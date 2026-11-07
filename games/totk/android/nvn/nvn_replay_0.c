/* NVN module: replay_0. */
#include "nvn_internal.h"

uint32_t nvn_translate_commands(const cb_t *cb, GfxCmd *out, uint32_t max,
                                       GfxOffscreenTarget *target,
                                       float clear[4], int *has_clear) {
    uint32_t n = 0;
    *has_clear = 0;
    int yflip = 0;
    if (!cb) return 0;
    nvn_render_state_t defaults;
    nvn_render_state_defaults(&defaults);
    GfxPipelineState cur_state;
    nvn_state_to_gfx(&defaults, &cur_state);
    for (uint32_t i = 0; i < cb->ncmds && n < max; i++) {
        const nvn_cmd_t *c = &cb->cmds[i];
        switch (c->kind) {
        case NVN_CMD_CLEAR_COLOR:
            if (c->u.clear.index != 0) {
                /* GfxCmd has one clear colour for the pass: the contract has no
                 * per-attachment index, so every attachment is cleared with
                 * attachment 0's value.  Different per-target colours need an
                 * additive contract field (reported to the lead). */
                static unsigned logged;
                if (logged++ < 8)
                    fprintf(stderr, "[totk-nvn] ClearColor target=%d folded into the pass clear"
                                    " (GfxCmd.clear has no attachment index)\n", c->u.clear.index);
                break;
            }
            memcpy(clear, c->u.clear.rgba, 4 * sizeof(float));
            *has_clear = 1;
            out[n].kind = GFX_CMD_CLEAR_COLOR;
            memcpy(out[n].u.clear.rgba, c->u.clear.rgba, 4 * sizeof(float));
            out[n].u.clear.mask = c->u.clear.mask;
            n++;
            break;
        case NVN_CMD_SET_VIEWPORT: {
            out[n].kind = GFX_CMD_SET_VIEWPORT;
            out[n].u.rect.x = c->u.rect.x;
            out[n].u.rect.y = c->u.rect.y;
            out[n].u.rect.width = c->u.rect.width;
            out[n].u.rect.height = c->u.rect.height;
            /* NVN's upper-left origin uses Y-up NDC; the Vulkan framebuffer
             * is Y-down.  A negative-height viewport is the faithful transform
             * (Vulkan 1.1 / VK_KHR_maintenance1). */
            const nvn_dev_t *dev = cb ? (const nvn_dev_t *)cb->dev : NULL;
            if (dev && dev->origin == 1 && out[n].u.rect.height > 0 &&
                gfx_backend_negative_viewport_supported()) {
                out[n].u.rect.y += out[n].u.rect.height;
                out[n].u.rect.height = -out[n].u.rect.height;
                yflip = 1;
            }
            n++;
            break;
        }
        case NVN_CMD_SET_SCISSOR:
            out[n].kind = GFX_CMD_SET_SCISSOR;
            out[n].u.rect.x = c->u.rect.x;
            out[n].u.rect.y = c->u.rect.y;
            out[n].u.rect.width = c->u.rect.width;
            out[n].u.rect.height = c->u.rect.height;
            n++;
            break;
        case NVN_CMD_BIND_STATE:
            nvn_state_to_gfx(&c->u.state.rs, &cur_state);
            break;
        case NVN_CMD_CLEAR_DEPTH_STENCIL:
            out[n].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
            out[n].u.clear_depth.depth = c->u.clear_ds.depth;
            out[n].u.clear_depth.dmask = c->u.clear_ds.dmask;
            out[n].u.clear_depth.stencil = c->u.clear_ds.stencil;
            out[n].u.clear_depth.smask = c->u.clear_ds.smask;
            n++;
            break;
        case NVN_CMD_BIND_DESC: {
            /* kind 3 is a storage-image bind: it has no graphics-pipeline
             * meaning, so it never enters the draw list. */
            if (c->u.desc.kind == 3) break;
            out[n].kind = GFX_CMD_BIND_DESC;
            out[n].u.bind.stage = c->u.desc.stage;
            out[n].u.bind.kind = c->u.desc.kind;
            out[n].u.bind.slot = c->u.desc.slot;
            if (c->u.desc.kind == 2) {
                uint64_t h = c->u.desc.handle;
                int tid = (int)(h & 0xfffff);
                int sid = (int)((h >> 20) & 0xfff);
                const tex_pool_t *tpool = nvn_tex_pool_for(cb);
                const samp_pool_t *spool = nvn_samp_pool_for(cb);
                const tex_t *t = (h && tid != 0xfffff && tpool)
                    ? nvn_tex_pool_lookup(tpool, tid) : NULL;
                const samp_t *sp = (h && spool)
                    ? nvn_samp_pool_lookup(spool, sid) : NULL;
                out[n].u.bind.texture = t ? nvn_texture_gfx_ready(t) : NULL;
                out[n].u.bind.sampler = sp ? nvn_sampler_gfx(sp) : NULL;
            } else {
                out[n].u.bind.data = (const void *)(uintptr_t)c->u.desc.address;
                out[n].u.bind.size = c->u.desc.size;
            }
            n++;
            break;
        }
        case NVN_CMD_DRAW:
            if (c->u.draw.instances > 1)
                nvn_fail("nvnQueueSubmitCommands", "instanced draw uses one instance for now");
            out[n].kind = GFX_CMD_DRAW;
            out[n].u.draw.primitive = c->u.draw.primitive;
            out[n].u.draw.first = c->u.draw.first;
            out[n].u.draw.count = c->u.draw.count;
            out[n].u.draw.state = cur_state;
            if (yflip) out[n].u.draw.state.front_face ^= 1;
            out[n].u.draw.has_state = 1;
            n++;
            break;
        case NVN_CMD_DRAW_INDEXED:
            out[n].kind = GFX_CMD_DRAW_INDEXED;
            out[n].u.draw_indexed.primitive = c->u.draw_indexed.primitive;
            out[n].u.draw_indexed.count = c->u.draw_indexed.count;
            out[n].u.draw_indexed.index_type = c->u.draw_indexed.index_type;
            out[n].u.draw_indexed.base_vertex = c->u.draw_indexed.base_vertex;
            out[n].u.draw_indexed.indices = c->u.draw_indexed.index_data
                ? (const void *)c->u.draw_indexed.index_data
                : (const void *)(uintptr_t)c->u.draw_indexed.indices;
            out[n].u.draw_indexed.index_size = (size_t)c->u.draw_indexed.count *
                (c->u.draw_indexed.index_type == 2 ? 4 :
                 c->u.draw_indexed.index_type == 1 ? 2 : 1);
            out[n].u.draw_indexed.state = cur_state;
            if (yflip) out[n].u.draw_indexed.state.front_face ^= 1;
            out[n].u.draw_indexed.has_state = 1;
            n++;
            break;
        case NVN_CMD_SET_RENDER_TARGETS:
            /* The host target is already the execute argument; emit the
             * explicit command when this pass has its own MRT image so the
             * backend binds exactly the resolved layout. */
            if (target) {
                out[n].kind = GFX_CMD_SET_RENDER_TARGETS;
                out[n].u.set_targets.target = target;
                n++;
            }
            break;
        default:
            break;
        }
    }
    return n;
}

/* Mip extent of a guest texture (block sizes are the backend's business). */
void nvn_level_extent(const tex_t *t, uint32_t level, uint32_t *w, uint32_t *h) {
    uint32_t lw = t ? t->d.width : 0, lh = t ? t->d.height : 0;
    for (uint32_t i = 0; i < level; i++) {
        lw = lw > 1 ? lw >> 1 : 1;
        lh = lh > 1 ? lh >> 1 : 1;
    }
    if (w) *w = lw;
    if (h) *h = lh;
}

/* Resolve a recorded region against the destination level.  NVN treats a
 * zero width/height as "to the edge of the level". */
void nvn_region_resolve(int32_t x, int32_t y, uint32_t *w, uint32_t *h,
                               uint32_t lw, uint32_t lh) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (!*w) *w = lw > (uint32_t)x ? lw - (uint32_t)x : 0;
    if (!*h) *h = lh > (uint32_t)y ? lh - (uint32_t)y : 0;
    if (*w && (uint32_t)x + *w > lw) *w = lw > (uint32_t)x ? lw - (uint32_t)x : 0;
    if (*h && (uint32_t)y + *h > lh) *h = lh > (uint32_t)y ? lh - (uint32_t)y : 0;
}

int nvn_exec_clear_texture(const nvn_cmd_t *cmd) {
    const void *texp = cmd->u.tex_clear.tex;
    const tex_t *t = nvn_tex_registry_find(texp);
    if (!t) {
        g_gpu_stats.clear_fail++;
        fprintf(stderr, "[totk-nvn] ClearTexture: texture %p is not a live NVN texture\n", texp);
        return 0;
    }
    uint32_t level = cmd->u.tex_clear.level, layer = cmd->u.tex_clear.layer;
    uint32_t lw = 0, lh = 0;
    nvn_level_extent(t, level, &lw, &lh);
    uint32_t w = cmd->u.tex_clear.width, h = cmd->u.tex_clear.height;
    int32_t x = cmd->u.tex_clear.x, y = cmd->u.tex_clear.y;
    nvn_region_resolve(x, y, &w, &h, lw, lh);
    if (!w || !h) {
        g_gpu_stats.clear_fail++;
        fprintf(stderr, "[totk-nvn] ClearTexture tex=%p level=%u: empty region %ux%u\n",
                texp, level, w, h);
        return 0;
    }
    const float *rgba = cmd->u.tex_clear.rgba;
    if (cmd->u.tex_clear.integer &&
        !(x == 0 && y == 0 && w >= lw && h >= lh)) {
        /* The frozen clear API takes float rgba.  A whole-level clear is exact
         * because VkClearColorValue is a union of the raw bits, but a
         * sub-region integer clear would be converted numerically. */
        g_gpu_stats.unsupported++;
        fprintf(stderr, "[totk-nvn] ClearTexture(i/ui) tex=%p sub-region %d,%d %ux%u:"
                        " the frozen API has no integer sub-region clear (not faked)\n",
                texp, x, y, w, h);
        return 0;
    }
    if (cmd->u.tex_clear.mask != 0xfu) {
        static unsigned mask_logged;
        if (mask_logged++ < 8)
            fprintf(stderr, "[totk-nvn] ClearTexture tex=%p mask=%#x: the backend clears"
                            " every channel (no per-channel clear mask in the frozen API)\n",
                    texp, cmd->u.tex_clear.mask);
    }
    GfxOffscreenTarget *rt = NULL;
    GfxRenderTargetLayout rtl;
    uint32_t rw = 0, rh = 0;
    if (nvn_tex_is_rt(t, &rt, &rtl, &rw, &rh)) {
        int full = x == 0 && y == 0 && w >= lw && h >= lh && level == 0 && layer == 0;
        if (!full) {
            g_gpu_stats.rt_unsupported++;
            fprintf(stderr, "[totk-nvn] ClearTexture tex=%p is a render target (%ux%u); a"
                            " partial clear (%d,%d %ux%u level=%u layer=%u) needs an"
                            " offscreen-region primitive the frozen API does not have\n",
                    texp, rw, rh, x, y, w, h, level, layer);
            return 0;
        }
        if (!gfx_backend_offscreen_clear(rt, rgba[0], rgba[1], rgba[2], rgba[3])) {
            g_gpu_stats.clear_fail++;
            fprintf(stderr, "[totk-nvn] ClearTexture: offscreen clear of rt=%p failed\n",
                    (void *)rt);
            return 0;
        }
        /* The sampled copy must be rebuilt from the render target. */
        nvn_tex_set_rt(t, rt, &rtl, rw, rh);
        g_gpu_stats.rt_clear_ok++;
        g_gpu_stats.clear_ok++;
        if (nvn_gpu_trace() || nvn_trace_frame())
            fprintf(stderr, "[totk-nvn] ClearTexture rt=%p %ux%u rgba=%.3f,%.3f,%.3f,%.3f"
                            " mask=%#x\n", (void *)rt, rw, rh, rgba[0], rgba[1], rgba[2],
                    rgba[3], cmd->u.tex_clear.mask);
        return 1;
    }
    GfxTexture *g = nvn_texture_gfx_ready(t);
    if (!g) {
        g_gpu_stats.clear_fail++;
        fprintf(stderr, "[totk-nvn] ClearTexture tex=%p: no host texture"
                        " (fmt=%#x %ux%u target=%d)\n", texp, t->d.format,
                t->d.width, t->d.height, t->d.target);
        return 0;
    }
    int ok = gfx_backend_texture_clear(g, level, layer, rgba, x, y, w, h);
    if (ok) g_gpu_stats.clear_ok++; else g_gpu_stats.clear_fail++;
    if (nvn_gpu_trace() || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] ClearTexture tex=%p gfx=%p level=%u layer=%u"
                        " %d,%d %ux%u rgba=%.3f,%.3f,%.3f,%.3f mask=%#x ok=%d\n",
                texp, (void *)g, level, layer, x, y, w, h, rgba[0], rgba[1], rgba[2],
                rgba[3], cmd->u.tex_clear.mask, ok);
    return ok;
}

int nvn_exec_copy_t2t(const nvn_cmd_t *cmd) {
    const void *srcp = cmd->u.tex_copy.src, *dstp = cmd->u.tex_copy.dst;
    const tex_t *src = nvn_tex_registry_find(srcp);
    const tex_t *dst = nvn_tex_registry_find(dstp);
    if (!src || !dst) {
        g_gpu_stats.copy_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToTexture: src=%p(%s) dst=%p(%s) not live\n",
                srcp, src ? "tex" : "?", dstp, dst ? "tex" : "?");
        return 0;
    }
    GfxOffscreenTarget *drt = NULL;
    if (nvn_tex_is_rt(dst, &drt, NULL, NULL, NULL)) {
        g_gpu_stats.rt_unsupported++;
        fprintf(stderr, "[totk-nvn] CopyTextureToTexture dst=%p is a render target (%p);"
                        " copying into it needs an offscreen write primitive the frozen"
                        " API does not have (source %p %#x %ux%u)\n", dstp, (void *)drt,
                srcp, src->d.format, src->d.width, src->d.height);
        return 0;
    }
    GfxTexture *sg = nvn_texture_gfx_ready(src);
    GfxTexture *dg = nvn_texture_gfx_ready(dst);
    if (!sg || !dg) {
        g_gpu_stats.copy_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToTexture: host texture missing src=%p dst=%p\n",
                (void *)sg, (void *)dg);
        return 0;
    }
    uint32_t slw = 0, slh = 0, dlw = 0, dlh = 0;
    nvn_level_extent(src, cmd->u.tex_copy.src_level, &slw, &slh);
    nvn_level_extent(dst, cmd->u.tex_copy.dst_level, &dlw, &dlh);
    uint32_t w = cmd->u.tex_copy.width, h = cmd->u.tex_copy.height;
    if (!w) w = slw < dlw ? slw : dlw;
    if (!h) h = slh < dlh ? slh : dlh;
    uint32_t d = cmd->u.tex_copy.depth;
    if (!d) d = 1;
    int ok = gfx_backend_texture_copy(dg, cmd->u.tex_copy.dst_level, cmd->u.tex_copy.dst_layer,
                                      cmd->u.tex_copy.dx, cmd->u.tex_copy.dy, cmd->u.tex_copy.dz,
                                      sg, cmd->u.tex_copy.src_level, cmd->u.tex_copy.src_layer,
                                      cmd->u.tex_copy.sx, cmd->u.tex_copy.sy, cmd->u.tex_copy.sz,
                                      w, h, d);
    if (ok) g_gpu_stats.copy_ok++; else g_gpu_stats.copy_fail++;
    if (nvn_gpu_trace() || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] CopyTextureToTexture src=%p(%#x %ux%u lv%u) dst=%p"
                        "(%#x %ux%u lv%u) src=%d,%d,%d dst=%d,%d,%d %ux%ux%u ok=%d\n",
                srcp, src->d.format, src->d.width, src->d.height, cmd->u.tex_copy.src_level,
                dstp, dst->d.format, dst->d.width, dst->d.height, cmd->u.tex_copy.dst_level,
                cmd->u.tex_copy.sx, cmd->u.tex_copy.sy, cmd->u.tex_copy.sz,
                cmd->u.tex_copy.dx, cmd->u.tex_copy.dy, cmd->u.tex_copy.dz, w, h, d, ok);
    return ok;
}

int nvn_exec_copy_b2t(const nvn_cmd_t *cmd) {
    const void *texp = cmd->u.buf_to_tex.tex;
    const tex_t *t = nvn_tex_registry_find(texp);
    if (!t) {
        g_gpu_stats.upload_fail++;
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture: dst texture %p not live\n", texp);
        return 0;
    }
    if (!cmd->u.buf_to_tex.data || !cmd->u.buf_to_tex.data_size) {
        g_gpu_stats.upload_fail++;
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture: dst=%p has no recorded payload\n", texp);
        return 0;
    }
    GfxOffscreenTarget *drt = NULL;
    if (nvn_tex_is_rt(t, &drt, NULL, NULL, NULL)) {
        g_gpu_stats.rt_unsupported++;
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture dst=%p is a render target (%p);"
                        " the frozen API has no offscreen upload primitive\n",
                texp, (void *)drt);
        return 0;
    }
    GfxTexture *g = nvn_texture_gfx_ready(t);
    if (!g) {
        g_gpu_stats.upload_fail++;
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture dst=%p: no host texture\n", texp);
        return 0;
    }
    uint32_t lw = 0, lh = 0;
    nvn_level_extent(t, cmd->u.buf_to_tex.level, &lw, &lh);
    uint32_t w = cmd->u.buf_to_tex.width, h = cmd->u.buf_to_tex.height;
    int32_t x = cmd->u.buf_to_tex.x, y = cmd->u.buf_to_tex.y;
    nvn_region_resolve(x, y, &w, &h, lw, lh);
    size_t bpp = nvn_texel_bytes(t);
    size_t need = (size_t)w * h * bpp;
    if (!w || !h || !bpp || !need) {
        g_gpu_stats.upload_fail++;
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture dst=%p: %ux%u bpp=%zu region invalid\n",
                texp, w, h, bpp);
        return 0;
    }
    size_t n = need < cmd->u.buf_to_tex.data_size ? need : cmd->u.buf_to_tex.data_size;
    int ok = gfx_backend_texture_upload(g, cmd->u.buf_to_tex.level, cmd->u.buf_to_tex.layer,
                                        x, y, w, h, cmd->u.buf_to_tex.data, n);
    if (ok) g_gpu_stats.upload_ok++; else g_gpu_stats.upload_fail++;
    if (nvn_gpu_trace() || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] CopyBufferToTexture dst=%p(%#x %ux%u lv%u) %d,%d %ux%u"
                        " bytes=%zu of %zu ok=%d\n", texp, t->d.format, t->d.width,
                t->d.height, cmd->u.buf_to_tex.level, x, y, w, h, n,
                cmd->u.buf_to_tex.data_size, ok);
    return ok;
}

int nvn_exec_copy_t2b(const nvn_cmd_t *cmd) {
    const void *texp = cmd->u.tex_to_buf.tex;
    const tex_t *t = nvn_tex_registry_find(texp);
    if (!t || !cmd->u.tex_to_buf.address) {
        g_gpu_stats.download_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToBuffer: tex=%p(%s) dst=%#llx invalid\n",
                texp, t ? "tex" : "?", (unsigned long long)cmd->u.tex_to_buf.address);
        return 0;
    }
    GfxTexture *g = nvn_texture_gfx_ready(t);
    if (!g) {
        g_gpu_stats.download_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToBuffer src=%p: no host texture\n", texp);
        return 0;
    }
    uint32_t lw = 0, lh = 0;
    nvn_level_extent(t, cmd->u.tex_to_buf.level, &lw, &lh);
    uint32_t w = cmd->u.tex_to_buf.width, h = cmd->u.tex_to_buf.height;
    int32_t x = cmd->u.tex_to_buf.x, y = cmd->u.tex_to_buf.y;
    nvn_region_resolve(x, y, &w, &h, lw, lh);
    size_t bpp = nvn_texel_bytes(t);
    size_t need = (size_t)w * h * bpp;
    if (!w || !h || !bpp || !need) {
        g_gpu_stats.download_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToBuffer src=%p: %ux%u bpp=%zu region invalid\n",
                texp, w, h, bpp);
        return 0;
    }
    uint8_t *tmp = malloc(need);
    if (!tmp) {
        g_gpu_stats.download_fail++;
        return 0;
    }
    int ok = gfx_backend_texture_download(g, cmd->u.tex_to_buf.level, cmd->u.tex_to_buf.layer,
                                          x, y, w, h, tmp, need);
    if (ok) {
        size_t n = need < cmd->u.tex_to_buf.size ? need : cmd->u.tex_to_buf.size;
        memcpy((void *)(uintptr_t)cmd->u.tex_to_buf.address, tmp, n);
        g_gpu_stats.download_ok++;
        if (nvn_gpu_trace() || nvn_trace_frame()) {
            uint32_t nz = nvn_first_nonzero(tmp, n, 1u << 20);
            fprintf(stderr, "[totk-nvn] CopyTextureToBuffer src=%p(%#x %ux%u lv%u) -> %#llx"
                            " %d,%d %ux%u bytes=%zu first_nz=%s%u\n", texp, t->d.format,
                    t->d.width, t->d.height, cmd->u.tex_to_buf.level,
                    (unsigned long long)cmd->u.tex_to_buf.address, x, y, w, h, n,
                    nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz);
        }
    } else {
        g_gpu_stats.download_fail++;
        fprintf(stderr, "[totk-nvn] CopyTextureToBuffer src=%p: download failed\n", texp);
    }
    free(tmp);
    return ok;
}
