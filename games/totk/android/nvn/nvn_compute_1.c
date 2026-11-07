/* NVN module: compute_1. */
#include "nvn_internal.h"

/* nvnCommandBufferDispatchCompute records the program and the descriptor
 * snapshot; the SPIR-V is loaded and the pipeline created at submit time. */
void nvn_trace_dispatch_compute(void *cv, int x, int y, int z) {
    cb_t *c = cv;
    nvn_api("nvnCommandBufferDispatchCompute");
    nvn_cb_dispatch_calls++;
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite dispatch compute %dx%dx%d\n", x, y, z);
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferDispatchCompute", "invalid command buffer");
        return;
    }
    if (x < 0 || y < 0 || z < 0) {
        nvn_fail("nvnCommandBufferDispatchCompute", "invalid group count");
        return;
    }
    if (!x || !y || !z) return;   /* NVN: an empty dispatch records nothing */
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DISPATCH);
    if (!cmd) return;
    cmd->u.dispatch.x = (uint32_t)x;
    cmd->u.dispatch.y = (uint32_t)y;
    cmd->u.dispatch.z = (uint32_t)z;
    cmd->u.dispatch.prog = c->prog;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) {
        nvn_dispatch_snap_t *snap = calloc(1, sizeof *snap);
        if (!snap) {
            nvn_fail("nvnCommandBufferDispatchCompute", "out of memory for bind snapshot");
            c->ncmds--;
            return;
        }
        snap->state = *b;
        /* Fill stage-5 slots the dispatch buffer did not rebind from the
         * queue-scoped tables, exactly like the graphics path inherits UBOs. */
        for (int slot = 0; slot < 16; slot++) {
            uint64_t ga = 0;
            size_t gn = 0;
            if (!(snap->state.ubo_mask[5] & (1u << slot)) &&
                nvn_global_ubo_get(5, slot, &ga, &gn) && ga && gn) {
                snap->state.ubo[5][slot].address = ga;
                snap->state.ubo[5][slot].size = gn;
                snap->state.ubo_mask[5] |= 1u << slot;
            }
            if (!(snap->state.ssbo_mask[5] & (1u << slot)) &&
                nvn_global_ssbo_get(5, slot, &ga, &gn) && ga && gn) {
                snap->state.ssbo[5][slot].address = ga;
                snap->state.ssbo[5][slot].size = gn;
                snap->state.ssbo_mask[5] |= 1u << slot;
            }
        }
        const tex_pool_t *pool = nvn_tex_pool_for(c);
        if (pool) {
            for (int slot = 0; slot < 16; slot++)
                if (snap->state.img_mask[5] & (1u << slot))
                    snap->img_tex[slot] =
                        nvn_tex_pool_lookup(pool, (int)(snap->state.img[5][slot] & 0xfffff));
            for (int slot = 0; slot < 128; slot++)
                if (snap->state.tex_mask[5][slot / 32] & (1u << (slot % 32)))
                    snap->tex_tex[slot] =
                        nvn_tex_pool_lookup(pool, (int)(snap->state.tex[5][slot] & 0xfffff));
        }
        cmd->u.dispatch.bind = snap;
    }
    prog_t *p = c->prog;
    if (p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G')) {
        pthread_mutex_lock(&g_prog_lock);
        prog_shader_state_t *s = prog_state_find(p);
        if (s) {
            s->refs++;
            cmd->u.dispatch.pstate = s;
        }
        pthread_mutex_unlock(&g_prog_lock);
    }
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] dispatch compute %dx%dx%d cb=%p prog=%p bind=%p\n",
                x, y, z, (void *)c, (void *)p, (void *)cmd->u.dispatch.bind);
}

void nvn_trace_dispatch_indirect(void *c, void *a) {
    (void)c; (void)a; nvn_zero_call("nvnCommandBufferDispatchComputeIndirect");
}

void nvn_trace_clear_texture(void *cv, void *t, void *v, void *r, void *color, int mask) {
    nvn_cb_clear_texture(cv, t, v, r, (const float *)color, mask, 0,
                         "nvnCommandBufferClearTexture");
}

/* Integer clear forms: the colour is int32/uint32[4]; keep the raw bits. */
void nvn_trace_clear_texturei(void *cv, void *t, void *v, void *r,
                                     void *color, int mask) {
    float rgba[4] = { 0, 0, 0, 0 };
    if (color) memcpy(rgba, color, sizeof rgba);
    nvn_cb_clear_texture(cv, t, v, r, rgba, mask, 1, "nvnCommandBufferClearTexturei");
}

void nvn_trace_clear_textureui(void *cv, void *t, void *v, void *r,
                                      void *color, int mask) {
    float rgba[4] = { 0, 0, 0, 0 };
    if (color) memcpy(rgba, color, sizeof rgba);
    nvn_cb_clear_texture(cv, t, v, r, rgba, mask, 1, "nvnCommandBufferClearTextureui");
}

/* nvnCommandBufferCopyBufferToTexture: the guest buffer can be recycled before
 * submit, so take a private copy of exactly the bytes the copy will consume. */
void nvn_trace_copy_b2t(void *cv, void *buf, void *t, void *v, void *r, int flags) {
    cb_t *c = cv;
    nvn_api("nvnCommandBufferCopyBufferToTexture");
    nvn_cb_copy_b2t++;
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite copy buffer->texture buf=%p t=%p\n", buf, t);
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferCopyBufferToTexture", "invalid command buffer");
        return;
    }
    const tex_t *tex = nvn_tex_registry_find(t);
    const buf_t *src = buf;
    if (!tex) {
        nvn_fail("nvnCommandBufferCopyBufferToTexture", "invalid destination texture");
        return;
    }
    if (!src || src->magic != NVN_MAGIC('B', 'U', 'F', 0) || !src->pool || !src->pool->mem) {
        nvn_fail("nvnCommandBufferCopyBufferToTexture", "invalid source buffer");
        return;
    }
    uint64_t address = (uint64_t)(uintptr_t)(src->pool->mem + src->off);
    const int32_t *reg = r;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_COPY_BUFFER_TO_TEXTURE);
    if (!cmd) return;
    cmd->u.buf_to_tex.tex = t;
    nvn_view_level_layer(v, &cmd->u.buf_to_tex.level, &cmd->u.buf_to_tex.layer);
    cmd->u.buf_to_tex.x = reg ? reg[0] : 0;
    cmd->u.buf_to_tex.y = reg ? reg[1] : 0;
    cmd->u.buf_to_tex.width = reg ? (uint32_t)(reg[3] > 0 ? reg[3] : 0) : 0;
    cmd->u.buf_to_tex.height = reg ? (uint32_t)(reg[4] > 0 ? reg[4] : 0) : 0;
    cmd->u.buf_to_tex.address = address;
    cmd->u.buf_to_tex.size = (size_t)src->size;
    cmd->u.buf_to_tex.flags = (uint32_t)flags;
    uint32_t lw = 0, lh = 0;
    nvn_level_extent(tex, cmd->u.buf_to_tex.level, &lw, &lh);
    uint32_t w = cmd->u.buf_to_tex.width, h = cmd->u.buf_to_tex.height;
    nvn_region_resolve(cmd->u.buf_to_tex.x, cmd->u.buf_to_tex.y, &w, &h, lw, lh);
    size_t need = (size_t)w * h * nvn_texel_bytes(tex);
    if (need && need > (256u << 20)) need = 0;
    if (need > (size_t)src->size) need = (size_t)src->size;
    if (need) {
        uint8_t *copy = malloc(need);
        if (!copy) {
            nvn_fail("nvnCommandBufferCopyBufferToTexture", "out of memory for payload copy");
            c->ncmds--;
            return;
        }
        memcpy(copy, (const void *)(uintptr_t)address, need);
        cmd->u.buf_to_tex.data = copy;
        cmd->u.buf_to_tex.data_size = need;
    }
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] copy-b2t cb=%p dst=%p(%#x %ux%u) region=%d,%d %ux%u"
                        " buf=%#llx size=%zu copied=%zu\n", (void *)c, t, tex->d.format,
                tex->d.width, tex->d.height, cmd->u.buf_to_tex.x, cmd->u.buf_to_tex.y,
                w, h, (unsigned long long)address, (size_t)src->size, need);
}

void nvn_trace_copy_t2t(void *cv, const void *src, const void *sv, const void *sr,
                               const void *dst, const void *dv, const void *dr, int flags) {
    cb_t *c = cv;
    nvn_api("nvnCommandBufferCopyTextureToTexture");
    nvn_cb_copy_t2t_calls++;
    if (nvn_trace_composite()) {
        const tex_t *s = nvn_tex_registry_find(src);
        const tex_t *d = nvn_tex_registry_find(dst);
        const int32_t *r = sr;
        fprintf(stderr, "[totk-frame] composite copy texture->texture src=%p(%s fmt=%#x %ux%u)"
                        " dst=%p(%s fmt=%#x %ux%u) region=%d,%d,%d %dx%dx%d\n",
                src, s ? "tex" : "?", s ? s->d.format : 0, s ? s->d.width : 0, s ? s->d.height : 0,
                dst, d ? "tex" : "?", d ? d->d.format : 0, d ? d->d.width : 0, d ? d->d.height : 0,
                r ? r[0] : 0, r ? r[1] : 0, r ? r[2] : 0,
                r ? r[3] : 0, r ? r[4] : 0, r ? r[5] : 0);
    }
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferCopyTextureToTexture", "invalid command buffer");
        return;
    }
    if (!nvn_tex_registry_find(src) || !nvn_tex_registry_find(dst)) {
        nvn_fail("nvnCommandBufferCopyTextureToTexture", "invalid source/destination texture");
        return;
    }
    const int32_t *sreg = sr, *dreg = dr;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_COPY_TEXTURE_TO_TEXTURE);
    if (!cmd) return;
    cmd->u.tex_copy.src = (void *)src;
    cmd->u.tex_copy.dst = (void *)dst;
    nvn_view_level_layer(sv, &cmd->u.tex_copy.src_level, &cmd->u.tex_copy.src_layer);
    nvn_view_level_layer(dv, &cmd->u.tex_copy.dst_level, &cmd->u.tex_copy.dst_layer);
    cmd->u.tex_copy.sx = sreg ? sreg[0] : 0;
    cmd->u.tex_copy.sy = sreg ? sreg[1] : 0;
    cmd->u.tex_copy.sz = sreg ? sreg[2] : 0;
    cmd->u.tex_copy.dx = dreg ? dreg[0] : 0;
    cmd->u.tex_copy.dy = dreg ? dreg[1] : 0;
    cmd->u.tex_copy.dz = dreg ? dreg[2] : 0;
    cmd->u.tex_copy.width = sreg ? (uint32_t)(sreg[3] > 0 ? sreg[3] : 0) : 0;
    cmd->u.tex_copy.height = sreg ? (uint32_t)(sreg[4] > 0 ? sreg[4] : 0) : 0;
    cmd->u.tex_copy.depth = sreg ? (uint32_t)(sreg[5] > 0 ? sreg[5] : 0) : 0;
    cmd->u.tex_copy.flags = (uint32_t)flags;
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] copy-t2t cb=%p src=%p dst=%p src=%d,%d,%d dst=%d,%d,%d"
                        " %ux%ux%u flags=%#x\n", (void *)c, src, dst,
                cmd->u.tex_copy.sx, cmd->u.tex_copy.sy, cmd->u.tex_copy.sz,
                cmd->u.tex_copy.dx, cmd->u.tex_copy.dy, cmd->u.tex_copy.dz,
                cmd->u.tex_copy.width, cmd->u.tex_copy.height, cmd->u.tex_copy.depth,
                cmd->u.tex_copy.flags);
}

/* nvnCommandBufferBlit has no exact mapping on the frozen API: its rectangle
 * lists and filter semantics are not a vkCmdCopyImage.  Keep it an explicit
 * unsupported report instead of pretending it ran. */
void nvn_trace_blit(void *cv, ...) {
    cb_t *c = cv;
    nvn_api("nvnCommandBufferBlit");
    nvn_cb_blit_calls++;
    nvn_gpu_report("Blit", NULL, "no exact copy semantics on the frozen API");
    static unsigned logged;
    if (logged++ < 16 || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] Blit cb=%p is unsupported (no exact copy semantics;"
                        " not faked) count=%u\n", cv, nvn_cb_blit_calls);
    if (c && c->magic != NVN_MAGIC('C', 'M', 'D', 'B'))
        nvn_fail("nvnCommandBufferBlit", "invalid command buffer");
}

/* nvnCommandBufferCopyTextureToBuffer: the guest buffer is the destination, so
 * it is written when the command runs (the download result is current then). */
void nvn_trace_copy_t2b(void *cv, void *t, void *v, void *r, void *buf, int flags) {
    cb_t *c = cv;
    nvn_api("nvnCommandBufferCopyTextureToBuffer");
    nvn_cb_copy_t2b_calls++;
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite copy texture->buffer t=%p buf=%p\n", t, buf);
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferCopyTextureToBuffer", "invalid command buffer");
        return;
    }
    const tex_t *tex = nvn_tex_registry_find(t);
    const buf_t *dst = buf;
    if (!tex) {
        nvn_fail("nvnCommandBufferCopyTextureToBuffer", "invalid source texture");
        return;
    }
    if (!dst || dst->magic != NVN_MAGIC('B', 'U', 'F', 0) || !dst->pool || !dst->pool->mem) {
        nvn_fail("nvnCommandBufferCopyTextureToBuffer", "invalid destination buffer");
        return;
    }
    const int32_t *reg = r;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_COPY_TEXTURE_TO_BUFFER);
    if (!cmd) return;
    cmd->u.tex_to_buf.tex = t;
    nvn_view_level_layer(v, &cmd->u.tex_to_buf.level, &cmd->u.tex_to_buf.layer);
    cmd->u.tex_to_buf.x = reg ? reg[0] : 0;
    cmd->u.tex_to_buf.y = reg ? reg[1] : 0;
    cmd->u.tex_to_buf.width = reg ? (uint32_t)(reg[3] > 0 ? reg[3] : 0) : 0;
    cmd->u.tex_to_buf.height = reg ? (uint32_t)(reg[4] > 0 ? reg[4] : 0) : 0;
    cmd->u.tex_to_buf.address = (uint64_t)(uintptr_t)(dst->pool->mem + dst->off);
    cmd->u.tex_to_buf.size = (size_t)dst->size;
    cmd->u.tex_to_buf.flags = (uint32_t)flags;
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] copy-t2b cb=%p src=%p(%#x %ux%u) region=%d,%d %ux%u"
                        " dst=%#llx size=%zu\n", (void *)c, t, tex->d.format, tex->d.width,
                tex->d.height, cmd->u.tex_to_buf.x, cmd->u.tex_to_buf.y,
                cmd->u.tex_to_buf.width, cmd->u.tex_to_buf.height,
                (unsigned long long)cmd->u.tex_to_buf.address, (size_t)dst->size);
}

void nvn_trace_cb_fence(void *c, void *sync, int condition, int flags) {
    (void)c; (void)sync; (void)condition; (void)flags;
    nvn_zero_call("nvnCommandBufferFenceSync");
}

void nvn_trace_cb_wait_sync(void *c, void *sync) {
    (void)c; (void)sync; nvn_zero_call("nvnCommandBufferWaitSync");
}

void nvn_trace_cb_signal_event(void *c, void *event, int value) {
    (void)c; (void)event; (void)value; nvn_zero_call("nvnCommandBufferSignalEvent");
}

void nvn_trace_cb_wait_event(void *c, void *event, int value, int flags) {
    (void)c; (void)event; (void)value; (void)flags; nvn_zero_call("nvnCommandBufferWaitEvent");
}

void nvn_trace_barrier(void *c, int bits) {
    (void)c; (void)bits; nvn_zero_call("nvnCommandBufferBarrier");
}

void nvn_trace_render_enable_cond(void *c, int mode, void *addr) {
    (void)c; (void)addr;
    nvn_zero_call("nvnCommandBufferSetRenderEnableConditional");
    if (nvn_trace_frame()) fprintf(stderr, "[totk-frame] render enable conditional mode=%d\n", mode);
}
