/* NVN module: bind. */
#include "nvn_internal.h"

static unsigned nvn_ubo_apply_trace;

static unsigned nvn_tex_apply_trace;

/* Replay a recorded command buffer into another one (Call/CopyCommands).
 * Command records are appended in order; the source's final bind state is
 * merged so the draw that follows sees the resources the copied commands
 * bound.  NVN applies those binds on the queue in the same order. */
void nvn_bind_state_merge(nvn_bind_state_t *d, const nvn_bind_state_t *s) {
    if (!d || !s) return;
    for (int st = 0; st < 6; st++) {
        for (int i = 0; i < 16; i++) {
            if (s->ubo_mask[st] & (1u << i)) {
                d->ubo[st][i] = s->ubo[st][i];
                d->ubo_mask[st] |= 1u << i;
            }
            if (s->ssbo_mask[st] & (1u << i)) {
                d->ssbo[st][i] = s->ssbo[st][i];
                d->ssbo_mask[st] |= 1u << i;
            }
        }
        for (int i = 0; i < 128; i++) {
            if (!(s->tex_mask[st][i / 32] & (1u << (i % 32)))) continue;
            d->tex[st][i] = s->tex[st][i];
            d->tex_mask[st][i / 32] |= 1u << (i % 32);
        }
        for (int i = 0; i < 16; i++) {
            if (!(s->img_mask[st] & (1u << i))) continue;
            d->img[st][i] = s->img[st][i];
            d->img_mask[st] |= 1u << i;
        }
    }
    if (s->va_mask) { memcpy(d->va, s->va, sizeof d->va); d->va_mask = s->va_mask; }
    if (s->vs_mask) { memcpy(d->vs, s->vs, sizeof d->vs); d->vs_mask = s->vs_mask; }
    if (s->vb_mask) {
        for (int i = 0; i < 16; i++)
            if (s->vb_mask & (1u << i)) d->vb[i] = s->vb[i];
        d->vb_mask |= s->vb_mask;
    }
    d->rs = s->rs;
    d->depth_tex = s->depth_tex;
}

/* Rewrite the translated texture binds so L3's per-draw descriptor epochs
 * match the shader's c2 slot, not the NVN BindTexture index. */
void nvn_remap_texture_bind_cmds(GfxCmd *cmds, uint32_t n,
                                        GfxReflectedPipeline *rp) {
    const nvn_pipe_ubo_t *ps = nvn_pipe_ubo_get(rp);
    if (!ps) return;
    for (int st = 0; st < 2; st++) {
        if (!ps->ntex[st] || ps->ntex[st] > 8) continue;
        uint32_t found[8], count = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (cmds[i].kind != GFX_CMD_BIND_DESC) continue;
            if (cmds[i].u.bind.stage != (uint32_t)st ||
                cmds[i].u.bind.kind != GFX_DESC_TEXTURE || !cmds[i].u.bind.texture) continue;
            if (count < 8) found[count] = i;
            count++;
        }
        if (count != ps->ntex[st]) continue;
        int compact = 1;
        for (uint32_t k = 0; k < count && k < 8; k++)
            if (cmds[found[k]].u.bind.slot >= ps->ntex[st]) compact = 0;
        if (!compact) continue;
        for (uint32_t k = 0; k < count && k < 8; k++) {
            int slot = ps->tex_slot[st][k];
            if (slot < 0) continue;
            if (cmds[found[k]].u.bind.slot == (uint32_t)slot) continue;
            if (nvn_trace_frame() || nvn_trace_inputs())
                fprintf(stderr, "[totk-frame] texture bind remap stage=%d index=%u -> slot=%d\n",
                        st, cmds[found[k]].u.bind.slot, slot);
            cmds[found[k]].u.bind.slot = (uint32_t)slot;
        }
    }
}

/* NVN UBO state is queue-scoped: the engine binds the per-frame table on a
 * state-only recording and draw buffers often carry only the changed slot.
 * Fill the compact indices the shader expects from the process-wide table. */
void nvn_inherit_ubo_slots(nvn_bind_state_t *b, GfxReflectedPipeline *rp) {
    if (!b || !rp) return;
    const nvn_pipe_ubo_t *ps = nvn_pipe_ubo_get(rp);
    if (!ps) return;
    for (int st = 0; st < 2; st++) {
        uint32_t n = ps->nubo[st] < 16 ? ps->nubo[st] : 16;
        for (uint32_t k = 0; k < n; k++) {
            if (b->ubo_mask[st] & (1u << k)) continue;
            uint64_t a = 0;
            size_t sz = 0;
            if (!nvn_global_ubo_get(st, (int)k, &a, &sz) || !a || !sz) continue;
            b->ubo[st][k].address = a;
            b->ubo[st][k].size = sz;
            b->ubo_mask[st] |= 1u << k;
        }
    }
}

void nvn_apply_bind_state(prog_t *p, GfxReflectedPipeline *rp,
                                 const nvn_bind_state_t *b, cb_t *cb) {
    (void)p;
    if (!b || !rp) return;
    memset(g_ubo_dec, 0, sizeof g_ubo_dec);
    const nvn_pipe_ubo_t *pspec = nvn_pipe_ubo_get(rp);
    for (int stage = 0; stage < 2; stage++) {
        uint32_t nspec = pspec ? pspec->nubo[stage] : 0;
        if (nvn_trace_first() && nvn_ubo_apply_trace++ < 8) {
            fprintf(stderr, "[totk-nvn] ApplyUbo stage=%d nbound_mask=%#x nspec=%u slots:",
                    stage, b->ubo_mask[stage], nspec);
            for (uint32_t k = 0; k < nspec; k++) fprintf(stderr, " %d", pspec->ubo_slot[stage][k]);
            fprintf(stderr, "\n");
        }
        int failed_slot = -1;
        const void *failed_data = NULL;
        size_t failed_size = 0;
        int nbound = 0;
        int bidx[16];
        const void *bdata[16];
        size_t bsize[16];
        for (int i = 0; i < 16; i++) {
            if (!(b->ubo_mask[stage] & (1u << i))) continue;
            const void *data = (const void *)(uintptr_t)b->ubo[stage][i].address;
            size_t size = b->ubo[stage][i].size;
            if (nbound < 16) { bidx[nbound] = i; bdata[nbound] = data; bsize[nbound] = size; }
            nbound++;
            failed_slot = i;
            failed_data = data;
            failed_size = size;
            if (!gfx_backend_reflected_bind_ubo_slot(rp, (uint32_t)stage, (uint32_t)i,
                                                     data, size)) {
                if (nvn_trace())
                    fprintf(stderr, "[totk-nvn] UBO stage=%d nvn index=%d size=%zu not in reflection\n",
                            stage, i, size);
            } else if (i < 16) {
                g_ubo_dec[stage][i] = (nvn_ubo_dec_t){ i, b->ubo[stage][i].address, size, 1, "snap" };
            }
        }
        /* NVN binds uniform buffers by the program's UBO index while the
         * translated SPIR-V records the constant-bank slot the shader reads.
         * When the stage has exactly one bound UBO and the shader has exactly
         * one UBO, they must be the same resource even if the numbers differ;
         * bind it to the reflected slot instead of silently leaving the shader
         * with the dummy buffer.  (Observed TOTK frame draw: NVN index 0,
         * shader cbuf slot 3.) */
        if (pspec && nbound > 1 && (uint32_t)nbound == nspec) {
            int compact = 1;
            for (int k = 0; k < nbound && k < 16; k++)
                if (bidx[k] >= nbound) compact = 0;
            if (compact) {
                for (int k = 0; k < nbound && k < 16; k++) {
                    int target = pspec->ubo_slot[stage][k];
                    if (target < 0 || target == bidx[k]) continue;
                    if (gfx_backend_reflected_bind_ubo_slot(rp, (uint32_t)stage,
                                                            (uint32_t)target,
                                                            bdata[k], bsize[k])) {
                        if (target >= 0 && target < 16)
                            g_ubo_dec[stage][target] = (nvn_ubo_dec_t){ bidx[k],
                                b->ubo[stage][bidx[k]].address, bsize[k], 1, "snap" };
                        if (nvn_trace_inputs()) {
                            static unsigned mlog;
                            if (mlog++ < 8)
                                fprintf(stderr, "[totk-frame] UBO stage=%d index=%d ->"
                                                " cbuf slot=%d (multi)\n",
                                        stage, bidx[k], target);
                        }
                    }
                }
            }
        }
        if (nbound == 1 && nspec == 1) {
            int target = pspec->ubo_slot[stage][0];
            /* The NVN bind address is the shader cbuf base: never rebase
             * onto an UpdateUniformBuffer subrange.  The update is replayed
             * into the guest buffer before translation, so a shader reading
             * rows 1..4 of a large support buffer sees the uploaded matrix at
             * its correct offset (L1 observed the 64-byte update at +16). */
            if (target >= 0 && target != failed_slot &&
                gfx_backend_reflected_bind_ubo_slot(rp, (uint32_t)stage, (uint32_t)target,
                                                    failed_data, failed_size)) {
                if (target < 16 && failed_slot >= 0 && failed_slot < 16)
                    g_ubo_dec[stage][target] = (nvn_ubo_dec_t){ failed_slot,
                        b->ubo[stage][failed_slot].address, failed_size, 1, "snap" };
                static unsigned logged;
                if (logged++ < 8)
                    fprintf(stderr, "[totk-nvn] UBO stage=%d nvn index=%d -> shader cbuf"
                                    " slot=%d (single-UBO program, size=%zu first=%08x"
                                    " %08x %08x %08x)\n",
                            stage, failed_slot, target, failed_size,
                            ((const uint32_t *)failed_data)[0], ((const uint32_t *)failed_data)[1],
                            ((const uint32_t *)failed_data)[2], ((const uint32_t *)failed_data)[3]);
            }
        }
        /* Per-program cbuf history: if this program bound a buffer for one of
         * its own shader slots while it was current, that is its c3 even after
         * another program overwrote the same NVN index.  This is what keeps the
         * window pass from inheriting a scene pass's projection buffer. */
        if (p && pspec) {
            for (uint32_t k = 0; k < pspec->nubo[stage] && k < 16; k++) {
                int slot = pspec->ubo_slot[stage][k];
                if (slot < 0 || slot >= 16) continue;
                uint64_t pa = 0;
                size_t pn = 0;
                if (!nvn_prog_ubo_get(p, stage, slot, &pa, &pn) || !pa || !pn) continue;
                if (g_ubo_dec[stage][slot].valid &&
                    g_ubo_dec[stage][slot].addr == pa &&
                    g_ubo_dec[stage][slot].size == pn) continue;
                if (!gfx_backend_reflected_bind_ubo_slot(rp, (uint32_t)stage, (uint32_t)slot,
                                                         (const void *)(uintptr_t)pa, pn))
                    continue;
                g_ubo_dec[stage][slot] = (nvn_ubo_dec_t){ slot, pa, pn, 1, "prog" };
                if (nvn_trace_composite()) {
                    static unsigned plog;
                    if (plog++ < 12)
                        fprintf(stderr, "[totk-frame] UBO stage=%d cbuf slot=%d <- program"
                                        " history (%#llx,%zu)\n", stage, slot,
                                (unsigned long long)pa, pn);
                }
            }
        }
        for (int i = 0; i < 16; i++) {
            if (!(b->ssbo_mask[stage] & (1u << i))) continue;
            const void *data = (const void *)(uintptr_t)b->ssbo[stage][i].address;
            if (!gfx_backend_reflected_bind_storage_slot(rp, (uint32_t)stage, (uint32_t)i,
                                                         data, b->ssbo[stage][i].size) &&
                nvn_trace())
                fprintf(stderr, "[totk-nvn] SSBO stage=%d slot=%d size=%zu not in reflection\n",
                        stage, i, b->ssbo[stage][i].size);
        }
        GfxTexture *bound_g[8];
        int bound_idx[8];
        int bound_tex = 0;
        for (int i = 0; i < 128; i++) {
            if (!(b->tex_mask[stage][i / 32] & (1u << (i % 32)))) continue;
            uint64_t handle = b->tex[stage][i];
            int tid = (int)(handle & 0xfffff);
            const tex_pool_t *tpool = nvn_tex_pool_for(cb);
            const tex_t *t = tpool ? nvn_tex_pool_lookup(tpool, tid) : NULL;
            if (!t) {
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_tex_get(stage, i, &gh, &gt) && gt && gh == handle) t = gt;
            }
            if ((nvn_trace_first() || (nvn_trace_frame() && nvn_scene_rendered > 0)) &&
                nvn_tex_apply_trace++ < 24)
                fprintf(stderr, "[totk-frame] ApplyTex stage=%d slot=%d handle=%#llx tid=%d"
                                " tpool=%p tex=%p\n", stage, i, (unsigned long long)handle,
                        tid, (const void *)tpool, (const void *)t);
            if (!t) continue;
            if (nvn_trace_inputs()) {
                static unsigned bind_traces;
                if (bind_traces++ < 8) {
                    size_t ms = t->L.size ? (size_t)t->L.size
                                          : (size_t)t->d.width * t->d.height * 4;
                    uint32_t nz = nvn_first_nonzero(t->mem, ms, 1u << 20);
                    fprintf(stderr, "[totk-frame] bind-tex stage=%d slot=%d handle=%#llx"
                                    " tex=%p fmt=%#x %ux%u lv=%u mem=%p msize=%zu"
                                    " first_nz=%s%u host=%p\n",
                            stage, i, (unsigned long long)handle, (const void *)t,
                            t->d.format, t->d.width, t->d.height, t->d.levels,
                            (void *)t->mem, ms, nz == UINT32_MAX ? "none " : "",
                            nz == UINT32_MAX ? 0 : nz, (void *)nvn_texture_gfx(t));
                }
            }
            GfxTexture *g = nvn_texture_gfx_ready(t);   /* lazy first-bind upload */
            if ((nvn_trace_first() || (nvn_trace_frame() && nvn_scene_rendered > 0)) &&
                nvn_tex_apply_trace <= 24)
                fprintf(stderr, "[totk-frame] ApplyTex stage=%d slot=%d gfx=%p target=%d"
                                " fmt=%#x %ux%u\n", stage, i, (void *)g, t->d.target,
                        t->d.format, t->d.width, t->d.height);
            if (!g) continue;
            if (bound_tex < 8) { bound_g[bound_tex] = g; bound_idx[bound_tex] = i; }
            bound_tex++;
            if (!gfx_backend_reflected_bind_texture_slot(rp, (uint32_t)stage, (uint32_t)i, g) &&
                nvn_trace())
                fprintf(stderr, "[totk-nvn] texture stage=%d slot=%d handle=%#llx not in reflection\n",
                        stage, i, (unsigned long long)handle);
        }
        /* NVN binds textures at compact descriptor indices while the shader
         * reads the c2 table slots the translator recorded (e.g. indices 0/1
         * vs slots 10/8).  When the counts match and every bound index is
         * compact, map the k-th bind to the k-th reflected texture slot. */
        if (nvn_trace_inputs() && stage == 1 && pspec && pspec->ntex[stage] > 1) {
            static unsigned mlog;
            if (mlog++ < 8)
                fprintf(stderr, "[totk-frame] apply-tex stage=1 rp=%p pspec=%p ntex=%u"
                                " bound=%d idx=%d,%d mask=%#x\n", (void *)rp,
                        (const void *)pspec, pspec ? pspec->ntex[stage] : 0,
                        bound_tex, bound_idx[0], bound_idx[1], b->tex_mask[stage][0]);
        }
        if (pspec && bound_tex > 0 && (uint32_t)bound_tex == pspec->ntex[stage]) {
            int compact = 1;
            for (int k = 0; k < bound_tex && k < 8; k++)
                if (bound_idx[k] >= (int)pspec->ntex[stage]) compact = 0;
            int ok_remap = 0;
            if (compact) {
                for (int k = 0; k < bound_tex && k < 8; k++) {
                    int target = pspec->tex_slot[stage][k];
                    if (target < 0) continue;
                    if (gfx_backend_reflected_bind_texture_slot(rp, (uint32_t)stage,
                                                               (uint32_t)target,
                                                               bound_g[k])) {
                        ok_remap++;
                        static unsigned logged;
                        if (logged++ < 12)
                            fprintf(stderr, "[totk-nvn] texture stage=%d index=%d ->"
                                            " shader slot=%d (multi)\n",
                                    stage, bound_idx[k], target);
                    }
                }
            }
            if (nvn_ui_seg_want() &&
                (bound_tex >= 2 || pspec->ntex[stage] != (uint32_t)bound_tex)) {
                static unsigned lim;
                if (lim++ < 64)
                    fprintf(stderr, "[totk-ui-seg] binds stage=%d rp=%p ntex=%u bound=%d"
                                    " idx=%d,%d slot=%d,%d compact=%d ok=%d g=%p,%p\n",
                            stage, (void *)rp, pspec->ntex[stage], bound_tex, bound_idx[0],
                            bound_idx[1], pspec->tex_slot[stage][0],
                            pspec->tex_slot[stage][1], compact, ok_remap,
                            (void *)bound_g[0], (void *)bound_g[1]);
            }
        } else if (nvn_ui_seg_want() && bound_tex >= 2) {
            static unsigned lim2;
            if (lim2++ < 64)
                fprintf(stderr, "[totk-ui-seg] binds stage=%d rp=%p ntex=%u bound=%d"
                                " pspec=%p (no remap) slots=%d,%d\n",
                        stage, (void *)rp, pspec ? pspec->ntex[stage] : 0, bound_tex,
                        (const void *)pspec, pspec ? pspec->tex_slot[stage][0] : -1,
                        pspec ? pspec->tex_slot[stage][1] : -1);
        }

    }
}

/* Translate the recorded NVN command order to the backend's clear/viewport/
 * scissor/draw list.  Resource binds stay in the reflected descriptor sets. */
void nvn_state_to_gfx(const nvn_render_state_t *rs, GfxPipelineState *o) {
    memset(o, 0, sizeof *o);
    o->blend_enable = rs->color.blend_en & 1;
    o->blend_src_rgb = rs->blend.src_rgb;
    o->blend_dst_rgb = rs->blend.dst_rgb;
    o->blend_src_a = rs->blend.src_a;
    o->blend_dst_a = rs->blend.dst_a;
    o->blend_eq_rgb = rs->blend.eq_rgb;
    o->blend_eq_a = rs->blend.eq_a;
    o->color_write_mask = rs->chmask.mask & 0xf;
    o->depth_test = rs->ds.dt;
    o->depth_write = rs->ds.dw;
    o->depth_func = rs->ds.df;
    o->stencil_test = rs->ds.st;
    o->stencil_func_front = rs->ds.sf_f;
    o->stencil_func_back = rs->ds.sf_b;
    o->stencil_fail_front = rs->ds.fail_f;
    o->stencil_zfail_front = rs->ds.zfail_f;
    o->stencil_pass_front = rs->ds.zpass_f;
    o->stencil_fail_back = rs->ds.fail_b;
    o->stencil_zfail_back = rs->ds.zfail_b;
    o->stencil_pass_back = rs->ds.zpass_b;
    o->cull_mode = rs->poly.cull;                  /* VkCullModeFlags values match */
    /* NVN uses the OpenGL convention: NDC has +Y up and front faces are CCW
     * in that space (NVN_FRONT_FACE_CCW = 1).  Vulkan's NDC and framebuffer
     * have +Y down, so the same vertex order has the opposite winding; map
     * NVN CCW to VkFrontFace CW or every NVN front face gets culled.  The
     * guest's own matrices are authored for NVN's Y-up clip space, so this is
     * the origin-mode transform, not a per-game flip. */
    o->front_face = rs->poly.front ? 1 : 0;        /* CCW=1 in Vulkan, CW=0 */
    o->polygon_mode = rs->poly.mode == 2 ? 0 : rs->poly.mode == 1 ? 1 : 2;
    o->stencil_ref = (uint32_t)rs->stencil_ref;
    memcpy(o->blend_color, rs->blend_color, sizeof o->blend_color);
}

int nvn_bs_samples_ui(const nvn_bind_state_t *b, cb_t *view,
                             char *out, size_t cap) {
    int n = 0;
    if (!b) return 0;
    const tex_pool_t *tp = nvn_tex_pool_for(view);
    for (int st = 0; st < 2; st++)
        for (int slot = 0; slot < 128; slot++) {
            if (!(b->tex_mask[st][slot / 32] & (1u << (slot % 32)))) continue;
            uint64_t h = b->tex[st][slot];
            const tex_t *t = tp ? nvn_tex_pool_lookup(tp, (int)(h & 0xfffff)) : NULL;
            if (!t) {
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_tex_get(st, slot, &gh, &gt) && gh == h) t = gt;
            }
            if (!nvn_tex_is_ui(t)) continue;
            n++;
            if (out && cap) {
                size_t used = strlen(out);
                snprintf(out + used, cap - used, " st%d:s%d=%p(%ux%u)",
                         st, slot, (const void *)t, t->d.width, t->d.height);
            }
        }
    return n;
}
