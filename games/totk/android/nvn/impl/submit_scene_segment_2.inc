/* nvn_submit_scene_snapshot: implementation phase 2. */
    /* Associate only after the segment actually drew: a queued segment whose
     * pipeline was missing must not become the compositor's sample source. */
    if (drew && ctex) nvn_tex_set_rt(ctex, rt, &layout, lw, lh);
    if (drew && lw >= 1024 && lh >= 1024) {
        g_last_scene_rt = rt;
        g_last_scene_w = lw;
        g_last_scene_h = lh;
        g_last_scene_tex = ctex;
        g_last_scene_color0 = sg->color0_tex;
        g_last_scene_layout = layout;
    }
    if (nvn_trace_frame() && lw <= 512) {
        static unsigned ui_log;
        if (ui_log++ < 4)
            fprintf(stderr, "[totk-frame] ui-seg %ux%u color0=%p program=%p texmask=%#x/%#x"
                            " ubomask=%#x/%#x va=%#x vb=%#x draws=%u\n",
                    lw, lh, sg->color0_tex, (void *)p, bs->tex_mask[0][0],
                    bs->tex_mask[1][0], bs->ubo_mask[0], bs->ubo_mask[1],
                    bs->va_mask, bs->vb_mask, sg->draw_count);
    }
    if (drew) {
        static unsigned ui_drawn;
        if (lw <= 512) ui_drawn++;
        uint8_t px[4] = { 0 };
        int have_px = gfx_backend_offscreen_read_pixel_index(rt, 0, lw / 2, lh / 2, px) != 0;
        if (have_px && nvn_trace_frame()) {
            static unsigned pxlog;
            if (pxlog++ < 6)
                fprintf(stderr, "[totk-frame] segment px %ux%u center=%02x%02x%02x%02x\n",
                        lw, lh, px[0], px[1], px[2], px[3]);
        }
        int ui_probe = lw <= 512 && (ui_drawn % 50 == 1) &&
                       (nvn_trace_inputs() || nvn_trace_frame());
        if (ui_probe) {
            size_t rn = 0;
            const uint8_t *rpix = gfx_backend_offscreen_pixels(rt, &rn);
            uint32_t rnz = nvn_first_nonzero(rpix, rn, 1u << 20);
            fprintf(stderr, "[totk-frame] ui-target n=%u %ux%u bytes=%zu first_nz=%s%u"
                            " center=%02x%02x%02x%02x\n", ui_drawn, lw, lh, rn,
                    rnz == UINT32_MAX ? "none " : "", rnz == UINT32_MAX ? 0 : rnz,
                    px[0], px[1], px[2], px[3]);
        }
        const char *seg_ppm = getenv("TOTK_ANDROID_SEG_PPM");
        if (seg_ppm && *seg_ppm && lw <= 512 && (ui_drawn % 50 == 1)) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s-%ux%u-%u.ppm", seg_ppm, lw, lh, ui_drawn);
            int ok = gfx_backend_offscreen_write_ppm_index(rt, 0, path);
            fprintf(stderr, "[totk-frame] segment target -> %s (%s)\n", path,
                    ok ? "ok" : "failed");
        }
        /* The 1920x1080 text/glyph composite: capture its offscreen image so
         * the vertex-stream fix can be judged by pixels, not only first_nz. */
        if (seg_ppm && *seg_ppm && lw >= 1024) {
            static unsigned big_ppm;
            uint8_t cpx[4] = { 0 };
            int have_px = gfx_backend_offscreen_read_pixel_index(rt, 0, lw / 2, lh / 2,
                                                                cpx) != 0;
            int nonzero = cpx[0] | cpx[1] | cpx[2] | cpx[3];
            if (have_px && nonzero && big_ppm < 4) {
                big_ppm++;
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s-text-%ux%u-%u.ppm", seg_ppm, lw, lh, big_ppm);
                int ok = gfx_backend_offscreen_write_ppm_index(rt, 0, path);
                fprintf(stderr, "[totk-frame] text target -> %s (%s)\n", path,
                        ok ? "ok" : "failed");
            }
        }
    }
    /* Same evidence for the large (window-sized) scene segments: the window
     * presents these targets, so a black scene has to be explained here. */
    if (nvn_trace_composite() && lw >= 1024 &&
        (sg->width >= 1024 || sg->height >= 1024) &&
        sg->layout.color_count >= 1 && sg->layout.color_formats[0] != 0) {
        static unsigned big_dumps;
        if (big_dumps < 8) {
            big_dumps++;
            uint32_t dcmd = 0, icmd = 0, prim = 0, cnt = 0, itype = 0;
            int vp[4] = { 0, 0, 0, 0 }, sc[4] = { 0, 0, 0, 0 };
            for (uint32_t ci = 0; ci < sg->ncmds; ci++) {
                const nvn_cmd_t *cc = &e->cmds[sg->first_cmd + ci];
                if (cc->kind == NVN_CMD_DRAW) {
                    dcmd++; prim = cc->u.draw.primitive; cnt += cc->u.draw.count;
                } else if (cc->kind == NVN_CMD_DRAW_INDEXED) {
                    icmd++; prim = cc->u.draw_indexed.primitive;
                    cnt += cc->u.draw_indexed.count; itype = cc->u.draw_indexed.index_type;
                } else if (cc->kind == NVN_CMD_SET_VIEWPORT) {
                    vp[0] = cc->u.rect.x; vp[1] = cc->u.rect.y;
                    vp[2] = cc->u.rect.width; vp[3] = cc->u.rect.height;
                } else if (cc->kind == NVN_CMD_SET_SCISSOR) {
                    sc[0] = cc->u.rect.x; sc[1] = cc->u.rect.y;
                    sc[2] = cc->u.rect.width; sc[3] = cc->u.rect.height;
                }
            }
            const tex_t *ot = nvn_tex_registry_find(sg->color0_tex);
            char vsk[20] = "-", psk[20] = "-";
            if (p) {
                pthread_mutex_lock(&g_prog_lock);
                prog_shader_state_t *ss = prog_state_find(p);
                if (ss) {
                    if (ss->key[0][0]) snprintf(vsk, sizeof vsk, "%.16s", ss->key[0]);
                    if (ss->key[1][0]) snprintf(psk, sizeof psk, "%.16s", ss->key[1]);
                }
                pthread_mutex_unlock(&g_prog_lock);
            }
            fprintf(stderr, "[totk-scene-seg] seg=%u target=%p fmt=%#x layout=%#x vs=%s"
                            " ps=%s %ux%u prog=%p rp=%p"
                            " warm=%d draws=%u dcmd=%u icmd=%u prim=%u cnt=%u itype=%u"
                            " vp=%d,%d %dx%d sc=%d,%d %dx%d texmask=%#x/%#x"
                            " vb=%#llx/%zu\n", seg_index, sg->color0_tex,
                    ot ? ot->d.format : 0, sg->layout.color_formats[0], vsk, psk,
                    lw, lh, (void *)p, (void *)rp, sg->warm,
                    sg->draw_count, dcmd, icmd, prim, cnt, itype, vp[0], vp[1], vp[2], vp[3],
                    sc[0], sc[1], sc[2], sc[3], bs->tex_mask[0][0], bs->tex_mask[1][0],
                    (unsigned long long)bs->vb[0].address, bs->vb[0].size);
            const tex_pool_t *tp2 = nvn_tex_pool_for(&view);
            for (int st = 0; st < 2; st++)
                for (int sl = 0; sl < 128; sl++) {
                    if (!(bs->tex_mask[st][sl / 32] & (1u << (sl % 32)))) continue;
                    uint64_t hh = bs->tex[st][sl];
                    const tex_t *tt = tp2 ? nvn_tex_pool_lookup(tp2,
                                            (int)(hh & 0xfffff)) : NULL;
                    if (!tt) {
                        uint64_t gh = 0;
                        const tex_t *gt = NULL;
                        if (nvn_global_tex_get(st, sl, &gh, &gt) && gh == hh) tt = gt;
                    }
                    size_t ms = tt ? (tt->L.size ? (size_t)tt->L.size
                                                 : (size_t)tt->d.width * tt->d.height * 4) : 0;
                    uint32_t nz = tt ? nvn_first_nonzero(tt->mem, ms, 1u << 20) : UINT32_MAX;
                    fprintf(stderr, "[totk-scene-seg]   tex st=%d slot=%d h=%#llx tex=%p"
                                    " fmt=%#x %ux%u first_nz=%s%u gfx=%p\n", st, sl,
                            (unsigned long long)hh, (const void *)tt,
                            tt ? (uint32_t)tt->d.format : 0, tt ? tt->d.width : 0,
                            tt ? tt->d.height : 0,
                            nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz,
                            tt ? (void *)nvn_texture_gfx(tt) : NULL);
                }
            for (int i = 0; i < 16; i++) {
                if (!g_ubo_dec[0][i].valid) continue;
                const uint32_t *w = (const uint32_t *)(uintptr_t)g_ubo_dec[0][i].addr;
                size_t nw = g_ubo_dec[0][i].size / 4;
                fprintf(stderr, "[totk-scene-seg]   c%d<=%s idx%d(%#llx,%zu)", i,
                        g_ubo_dec[0][i].src ? g_ubo_dec[0][i].src : "?",
                        g_ubo_dec[0][i].nvn, (unsigned long long)g_ubo_dec[0][i].addr,
                        g_ubo_dec[0][i].size);
                if (w && nw >= 20) {
                    fprintf(stderr, " rows1..4=%08x,%08x,%08x,%08x | %08x,%08x,%08x,%08x"
                                    " | %08x,%08x,%08x,%08x | %08x,%08x,%08x,%08x",
                            w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                            w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19]);
                }
                fputc('\n', stderr);
            }
            size_t rn = 0;
            const uint8_t *rpix = gfx_backend_offscreen_pixels(rt, &rn);
            uint32_t rnz = nvn_first_nonzero(rpix, rn, 1u << 20);
            fprintf(stderr, "[totk-scene-seg] result seg=%u %ux%u drew=%d bytes=%zu"
                            " first_nz=%s%u\n", seg_index, lw, lh, drew, rn,
                    rnz == UINT32_MAX ? "none " : "", rnz == UINT32_MAX ? 0 : rnz);
        }
    }
    /* Lane 2: the exact 360x69 output of each of the first UI segments, so the
     * first writer in the chain can be identified (not just the 50th).  The
     * raw RGBA8 readback also reports alpha coverage and the RGB colour count:
     * a PPM drops alpha, so a "drawn but transparent" target looks identical
     * to a black one. */
    if (nvn_ui_seg_want() && lw <= 512) {
        static unsigned ui_seg_dumps;
        if (ui_seg_dumps < 12) {
            size_t rn = 0;
            const uint8_t *rpix = gfx_backend_offscreen_pixels(rt, &rn);
            uint32_t rnz = nvn_first_nonzero(rpix, rn, 1u << 20);
            uint32_t npx = (uint32_t)(rn / 4);
            uint32_t nz_a = 0, amin = 255, amax = 0, a0 = 0;
            for (uint32_t i = 0; i < npx; i++) {
                uint32_t a = rpix[i * 4 + 3];
                if (!a) { if (!a0) a0 = 1; continue; }
                nz_a++;
                if (a < amin) amin = a;
                if (a > amax) amax = a;
            }
            /* Distinct RGB (24-bit) colours, capped so the scan stays cheap. */
            uint32_t cols[64];
            uint32_t ncol = 0;
            for (uint32_t i = 0; i < npx && ncol < 64; i++) {
                uint32_t c = ((uint32_t)rpix[i * 4] << 16) |
                             ((uint32_t)rpix[i * 4 + 1] << 8) | rpix[i * 4 + 2];
                uint32_t j = 0;
                for (; j < ncol; j++) if (cols[j] == c) break;
                if (j == ncol) cols[ncol++] = c;
            }
            uint32_t ca = npx ? rpix[(size_t)(lh / 2) * lw * 4 + (size_t)(lw / 2) * 4 + 3] : 0;
            fprintf(stderr, "[totk-ui-seg] result seg=%u %ux%u drew=%d bytes=%zu"
                            " first_nz=%s%u alpha_nz=%u/%u alpha=%u..%u centre_a=%u"
                            " rgb_colours=%u%s\n", seg_index, lw, lh, drew, rn,
                    rnz == UINT32_MAX ? "none " : "", rnz == UINT32_MAX ? 0 : rnz,
                    nz_a, npx, nz_a ? amin : 0, amax, ca, ncol,
                    a0 ? "" : " alpha-all-zero");
            const char *ui_seg_ppm = getenv("TOTK_ANDROID_UI_SEG_PPM");
            if (ui_seg_ppm && *ui_seg_ppm) {
                char path[PATH_MAX];
                snprintf(path, sizeof path, "%s-s%u-%ux%u.ppm", ui_seg_ppm,
                         seg_index, lw, lh);
                int ok = gfx_backend_offscreen_write_ppm_index(rt, 0, path);
                fprintf(stderr, "[totk-ui-seg] ppm seg=%u -> %s (%s)\n", seg_index,
                        path, ok ? "ok" : "failed");
            }
            ui_seg_dumps++;
        }
    }
    e->age = ++g_retired_age;
    nvn_scene_submits++;
    if (drew) nvn_scene_rendered++;
    uint32_t ncmds = sg->ncmds, ndraws = sg->draw_count;
    uint32_t first = sg->first_cmd;
    const void *ctex_dbg = sg->color0_tex;
    prog_shader_state_t *sg_pstate = sg->pstate;
    int sg_warm = sg->warm;
    const char *scene_ppm = getenv("TOTK_ANDROID_SCENE_PPM");
    if (drew && scene_ppm && *scene_ppm) {
        static int dumped;
        if (!dumped) {
            dumped = 1;
            int ok = gfx_backend_offscreen_write_ppm_index(rt, 0, scene_ppm);
            fprintf(stderr, "[totk-frame] scene target -> %s (%s)\n", scene_ppm,
                    ok ? "ok" : "failed");
        }
    }
    int vp[4] = { 0, 0, 0, 0 }, sc[4] = { 0, 0, 0, 0 };
    uint32_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
    uint32_t dbg_tex0 = 0, dbg_tex1 = 0;
    uint32_t ndraw_cmd = 0, nidx_cmd = 0, idx_type = 0, idx_count = 0;
    uint32_t idx_prim = 0;
    uint64_t idx_ptr = 0;
    uint32_t idxv[6] = { 0, 0, 0, 0, 0, 0 };
    if (nvn_ui_seg_want()) {
        /* Compact per-segment geometry/state evidence: a segment can "draw"
         * (the backend call succeeds) and still cover no pixels when its
         * viewport, scissor or vertex stream is wrong. */
        for (uint32_t ci = 0; ci < sg->ncmds; ci++) {
            const nvn_cmd_t *cc = &e->cmds[sg->first_cmd + ci];
            if (cc->kind == NVN_CMD_SET_VIEWPORT) {
                vp[0] = cc->u.rect.x; vp[1] = cc->u.rect.y;
                vp[2] = cc->u.rect.width; vp[3] = cc->u.rect.height;
            } else if (cc->kind == NVN_CMD_SET_SCISSOR) {
                sc[0] = cc->u.rect.x; sc[1] = cc->u.rect.y;
                sc[2] = cc->u.rect.width; sc[3] = cc->u.rect.height;
            } else if (cc->kind == NVN_CMD_DRAW) {
                ndraw_cmd++;
            } else if (cc->kind == NVN_CMD_DRAW_INDEXED) {
                if (!nidx_cmd) {
                    idx_type = cc->u.draw_indexed.index_type;
                    idx_count = cc->u.draw_indexed.count;
                    idx_prim = cc->u.draw_indexed.primitive;
                    idx_ptr = cc->u.draw_indexed.indices;
                    const uint8_t *src = cc->u.draw_indexed.index_data
                        ? cc->u.draw_indexed.index_data
                        : (const uint8_t *)(uintptr_t)cc->u.draw_indexed.indices;
                    for (uint32_t k = 0; k < 6 && k < idx_count && src; k++) {
                        if (idx_type == 0) idxv[k] = src[k];
                        else if (idx_type == 1) { uint16_t v = 0; memcpy(&v, src + k * 2, 2); idxv[k] = v; }
                        else { uint32_t v = 0; memcpy(&v, src + k * 4, 4); idxv[k] = v; }
                    }
                }
                nidx_cmd++;
            }
        }
        dbg_tex0 = bs->tex_mask[0][0];
        dbg_tex1 = bs->tex_mask[1][0];
        if (nvn_ui_seg_want() && seg_index <= 3) {
            GfxVertexLayout dl;
            uint64_t dsig = 0;
            nvn_build_vertex_layout(bs, &dl, &dsig);
            char tag[32];
            snprintf(tag, sizeof tag, "uiseg%u", seg_index);
            nvn_dump_vertex_stream(tag, &dl, bs, 4);
        }
        /* Index-range check: does the guest ring buffer still hold the bytes
         * that were recorded?  A deferred segment must use the snapshot. */
        for (uint32_t ci = 0; ci < sg->ncmds; ci++) {
            const nvn_cmd_t *cc = &e->cmds[sg->first_cmd + ci];
            if (cc->kind != NVN_CMD_DRAW_INDEXED) continue;
            if (cc->u.draw_indexed.index_data && cc->u.draw_indexed.index_size >= 4) {
                uint32_t now = 0;
                memcpy(&now, (const void *)(uintptr_t)cc->u.draw_indexed.indices, 4);
                uint32_t was = 0;
                memcpy(&was, cc->u.draw_indexed.index_data, 4);
                if (now != was)
                    fprintf(stderr, "[totk-ui-seg] IDX CHANGED seg=%u type=%u count=%u"
                                    " addr=%#llx now=%08x was=%08x\n", seg_index,
                            cc->u.draw_indexed.index_type, cc->u.draw_indexed.count,
                            (unsigned long long)cc->u.draw_indexed.indices, now, was);
            }
            break;
        }
        if ((bs->vb_mask & 1u) && bs->vb[0].address) {
            const uint8_t *v = (const uint8_t *)(uintptr_t)bs->vb[0].address;
            memcpy(&v0, v, 4); memcpy(&v1, v + 4, 4);
            memcpy(&v2, v + 8, 4); memcpy(&v3, v + 12, 4);
            if (sg->have_vb_head && memcmp(sg->vb_head, v, sizeof sg->vb_head) != 0) {
                uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0;
                memcpy(&h0, sg->vb_head, 4); memcpy(&h1, sg->vb_head + 4, 4);
                memcpy(&h2, sg->vb_head + 8, 4); memcpy(&h3, sg->vb_head + 12, 4);
                fprintf(stderr, "[totk-ui-seg] VB CHANGED seg=%u addr=%#llx now="
                                "%08x %08x %08x %08x was=%08x %08x %08x %08x\n",
                        seg_index, (unsigned long long)bs->vb[0].address,
                        v0, v1, v2, v3, h0, h1, h2, h3);
            }
        }
    }
    if (nvn_ui_seg_want()) {
        /* Which NVN uniform buffer fed each shader cbuf slot, plus the rows the
         * UI vertex shader actually reads (vp_c3 rows 1..6 and 33). */
        fprintf(stderr, "[totk-ui-seg] ubo seg=%u slots:", seg_index);
        for (int i = 0; i < 16; i++)
            if (g_ubo_dec[0][i].valid)
                fprintf(stderr, " c%d<=idx%d(%#llx,%zu)", i, g_ubo_dec[0][i].nvn,
                        (unsigned long long)g_ubo_dec[0][i].addr, g_ubo_dec[0][i].size);
        for (int i = 0; i < 4; i++) {
            uint64_t ga = 0;
            size_t gn = 0;
            if (nvn_global_ubo_get(0, i, &ga, &gn))
                fprintf(stderr, " glob%d=(%#llx,%zu)", i, (unsigned long long)ga, gn);
        }
        fputc('\n', stderr);
        for (int i = 0; i < 16; i++) {
            if (!g_ubo_dec[0][i].valid) continue;
            const uint32_t *w = (const uint32_t *)(uintptr_t)g_ubo_dec[0][i].addr;
            size_t nw = g_ubo_dec[0][i].size / 4;
            if (!w || nw < 16) continue;
            static const int rows[7] = { 1, 2, 3, 4, 5, 6, 33 };
            fprintf(stderr, "[totk-ui-seg] ubo seg=%u c%d rows:", seg_index, i);
            for (int r = 0; r < 7; r++) {
                int base = rows[r] * 4;
                if ((size_t)base + 4 > nw) { fprintf(stderr, " %d=oob", rows[r]); continue; }
                fprintf(stderr, " %d=%08x,%08x,%08x,%08x", rows[r],
                        w[base], w[base + 1], w[base + 2], w[base + 3]);
            }
            fputc('\n', stderr);
        }
    }
    pthread_mutex_unlock(&g_retired_lock);
    if (nvn_ui_seg_want())
        fprintf(stderr, "[totk-ui-seg] run handle=%#llx seg=%u %ux%u draws=%u rp=%p"
                        " pstate=%p warm=%d drew=%d vp=%d,%d %dx%d sc=%d,%d %dx%d"
                        " tex=%#x/%#x dcmd=%u icmd=%u itype=%u iprim=%u icount=%u iptr=%#llx"
                        " idx=%u,%u,%u,%u,%u,%u vb=%#llx/%zu tl=%u/%u/%u"
                        " v0=%08x %08x %08x %08x\n",
                (unsigned long long)handle, seg_index, lw, lh, ndraws, (void *)rp,
                (void *)sg_pstate, sg_warm, drew,
                vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3],
                dbg_tex0, dbg_tex1, ndraw_cmd, nidx_cmd, idx_type, idx_prim, idx_count,
                (unsigned long long)idx_ptr, idxv[0], idxv[1], idxv[2], idxv[3],
                idxv[4], idxv[5],
                (unsigned long long)bs->vb[0].address, bs->vb[0].size,
                tl_count, tl_streams, tl_stream0,
                v0, v1, v2, v3);
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] scene submit handle=%#llx seg=%u rt=%p %ux%u"
                        " color0=%p cmd=[%u,%u) draws=%u program=%p pipeline=%p"
                        " drew=%d total=%u/%u\n",
                (unsigned long long)handle, seg_index, (void *)rt, lw, lh,
                ctex_dbg, first, first + ncmds, ndraws, (void *)p, (void *)rp, drew,
                nvn_scene_rendered, nvn_scene_submits);
    return drew;
