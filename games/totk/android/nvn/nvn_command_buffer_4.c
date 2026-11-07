/* NVN module: command_buffer_4. */
#include "nvn_internal.h"

void nvnCommandBufferDrawElementsBaseVertex(cb_t *c, int mode, int type, int count,
                                                   uint64_t index, int base_vertex) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferDrawElementsBaseVertex", "invalid command buffer"); return; }
    if (count < 0 || !index) { nvn_fail("nvnCommandBufferDrawElementsBaseVertex", "invalid count/index"); return; }
    if (type < 0 || type > 2) { nvn_fail("nvnCommandBufferDrawElementsBaseVertex", "invalid index type"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW_INDEXED);
    if (cmd) {
        cmd->u.draw_indexed.primitive = (uint32_t)mode;
        cmd->u.draw_indexed.count = (uint32_t)count;
        cmd->u.draw_indexed.index_type = (uint32_t)type;
        cmd->u.draw_indexed.base_vertex = (uint32_t)base_vertex;
        cmd->u.draw_indexed.indices = index;
        nvn_cb_snapshot_indices(cmd, type, count, index);
        nvn_cb_snapshot_vertex_buffers(cmd, c->bind_state,
                                       nvn_cb_index_vertex_count(cmd));
    }
    c->draw_count += count;
    nvn_cb_trace_record_vb(c, "elements-base", (uint32_t)count);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] DrawElementsBaseVertex mode=%d type=%d count=%d index=%#llx base=%d\n",
                mode, type, count, (unsigned long long)index, base_vertex);
}

/* NVN descriptor state.  The shader reflection names a NVN slot (cbufs/sbufs)
 * or a c[2] handle (textures/images); these calls are the source of the actual
 * resources.  Drawing still uses the host pipeline until the guest reaches the
 * nn::gfx context path, but the state is kept and traced so a future bind is
 * not another guess. */
void nvnCommandBufferSetTexturePool(cb_t *c, const tex_pool_t *p) {
    nvn_api("nvnCommandBufferSetTexturePool");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetTexturePool", "invalid command buffer"); return; }
    if (p && p->magic != NVN_MAGIC('T', 'P', 'O', 'L')) { nvn_fail("nvnCommandBufferSetTexturePool", "invalid texture pool"); return; }
    c->tex_pool = p;
    if (p) { pthread_mutex_lock(&g_pool_lock); g_last_tex_pool = p; pthread_mutex_unlock(&g_pool_lock); }
    if (nvn_trace() || (nvn_trace_first()))
        fprintf(stderr, "[totk-nvn] SetTexturePool cb=%p pool=%p n=%d rec=%u\n",
                (void *)c, (const void *)p, p ? (int)p->n : 0, c->recording);
}

void nvnCommandBufferSetSamplerPool(cb_t *c, const samp_pool_t *p) {
    nvn_api("nvnCommandBufferSetSamplerPool");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetSamplerPool", "invalid command buffer"); return; }
    if (p && p->magic != NVN_MAGIC('S', 'P', 'O', 'L')) { nvn_fail("nvnCommandBufferSetSamplerPool", "invalid sampler pool"); return; }
    c->samp_pool = p;
    if (p) { pthread_mutex_lock(&g_pool_lock); g_last_samp_pool = p; pthread_mutex_unlock(&g_pool_lock); }
    if (nvn_trace() || (nvn_trace_first()))
        fprintf(stderr, "[totk-nvn] SetSamplerPool cb=%p pool=%p n=%d rec=%u\n",
                (void *)c, (const void *)p, p ? (int)p->n : 0, c->recording);
}

/* NVN copies the payload into command memory when recording and writes it to
 * the buffer when the command buffer runs, so the caller may reuse/change the
 * source afterwards.  The CPU translator does not have GPU command memory, so
 * keep a private copy in the command record and replay it at submit time. */
void nvnCommandBufferUpdateUniformBuffer(cb_t *c, uint64_t buffer, size_t buffer_size,
                                                ptrdiff_t offset, size_t size, const void *data) {
    nvn_api("nvnCommandBufferUpdateUniformBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferUpdateUniformBuffer", "invalid command buffer"); return; }
    if (!buffer || !data || !size) return;
    if (offset < 0 || (uint64_t)offset > buffer_size || size > buffer_size - (uint64_t)offset) {
        nvn_fail("nvnCommandBufferUpdateUniformBuffer", "range outside buffer");
        return;
    }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_UPDATE_UBO);
    if (!cmd) return;
    uint8_t *copy = malloc(size);
    if (!copy) { nvn_fail("nvnCommandBufferUpdateUniformBuffer", "out of memory"); c->ncmds--; return; }
    memcpy(copy, data, size);
    cmd->u.update.dst = buffer + (uint64_t)offset;
    cmd->u.update.size = (uint32_t)size;
    cmd->u.update.data = copy;
    if (nvn_trace_frame()) {
        static unsigned updates;
        if (updates++ < 16) {
            fprintf(stderr, "[totk-frame] UpdateUniformBuffer cb=%p dst=%#llx off=%lld size=%zu data=",
                    (void *)c, (unsigned long long)cmd->u.update.dst, (long long)offset, size);
            size_t words = size / 4; if (words > 16) words = 16;
            for (size_t k = 0; k < words; k++) fprintf(stderr, " %08x", ((const uint32_t *)data)[k]);
            fputc('\n', stderr);
        }
    }
}

void nvnCommandBufferBindUniformBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindUniformBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindUniformBuffer", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 16) { nvn_fail("nvnCommandBufferBindUniformBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    if (!address || !size) {
        b->ubo[stage][i].address = 0;
        b->ubo[stage][i].size = 0;
        b->ubo_mask[stage] &= ~(1u << i);
        nvn_global_ubo_set(stage, i, 0, 0);
        nvn_cb_desc_event(c, (uint32_t)stage, 0, (uint32_t)i, 0, 0, 0);
        return;
    }
    b->ubo[stage][i].address = address;
    b->ubo[stage][i].size = size;
    b->ubo_mask[stage] |= 1u << i;
    nvn_global_ubo_set(stage, i, address, size);
    nvn_prog_ubo_set(c->prog, stage, i, address, size);
    nvn_cb_desc_event(c, (uint32_t)stage, 0, (uint32_t)i, address, size, 0);
    if (nvn_trace() || ((nvn_trace_first() || nvn_trace_inputs()) && nvn_ubo_trace++ < 8192)) {
        const uint32_t *u = (const uint32_t *)(uintptr_t)address;
        fprintf(stderr, "[totk-nvn] BindUniformBuffer cb=%p rec=%u stage=%d index=%d addr=%#llx size=%zu",
                (void *)c, c->recording, stage, i, (unsigned long long)address, size);
        for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", u[k]);
        fprintf(stderr, "\n");
    }
}

void nvnCommandBufferBindUniformBuffers(cb_t *c, int stage, int first, int n, const void *ranges) {
    nvn_api("nvnCommandBufferBindUniformBuffers");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindUniformBuffers", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || first < 0 || n < 0 || first + n > 16 || (!ranges && n)) { nvn_fail("nvnCommandBufferBindUniformBuffers", "invalid range"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    const uint64_t *r = ranges;
    for (int i = 0; i < n; i++) {
        b->ubo[stage][first + i].address = r[i * 2];
        b->ubo[stage][first + i].size = (size_t)r[i * 2 + 1];
        b->ubo_mask[stage] |= 1u << (first + i);
        nvn_global_ubo_set(stage, first + i, r[i * 2], (size_t)r[i * 2 + 1]);
        nvn_prog_ubo_set(c->prog, stage, first + i, r[i * 2], (size_t)r[i * 2 + 1]);
        nvn_cb_desc_event(c, (uint32_t)stage, 0, (uint32_t)(first + i),
                          r[i * 2], (size_t)r[i * 2 + 1], 0);
    }
    if (nvn_trace() || ((nvn_trace_first() || nvn_trace_inputs()) && nvn_ubo_trace++ < 8192)) {
        fprintf(stderr, "[totk-nvn] BindUniformBuffers cb=%p rec=%u stage=%d first=%d count=%d",
                (void *)c, c->recording, stage, first, n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, " [%d]=%#llx+%llu", first + i,
                    (unsigned long long)r[i * 2], (unsigned long long)r[i * 2 + 1]);
        fprintf(stderr, "\n");
    }
}

void nvnCommandBufferBindStorageBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindStorageBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindStorageBuffer", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 16 || !address || !size) { nvn_fail("nvnCommandBufferBindStorageBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    b->ssbo[stage][i].address = address;
    b->ssbo[stage][i].size = size;
    b->ssbo_mask[stage] |= 1u << i;
    nvn_global_ssbo_set(stage, i, address, size);
    nvn_cb_desc_event(c, (uint32_t)stage, 1, (uint32_t)i, address, size, 0);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindStorageBuffer stage=%d index=%d addr=%#llx size=%zu\n",
                stage, i, (unsigned long long)address, size);
}

void nvnCommandBufferBindStorageBuffers(cb_t *c, int stage, int first, int n, const void *ranges) {
    nvn_api("nvnCommandBufferBindStorageBuffers");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindStorageBuffers", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || first < 0 || n < 0 || first + n > 16 || (!ranges && n)) { nvn_fail("nvnCommandBufferBindStorageBuffers", "invalid range"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    const uint64_t *r = ranges;
    for (int i = 0; i < n; i++) {
        b->ssbo[stage][first + i].address = r[i * 2];
        b->ssbo[stage][first + i].size = (size_t)r[i * 2 + 1];
        b->ssbo_mask[stage] |= 1u << (first + i);
        nvn_global_ssbo_set(stage, first + i, r[i * 2], (size_t)r[i * 2 + 1]);
        nvn_cb_desc_event(c, (uint32_t)stage, 1, (uint32_t)(first + i),
                          r[i * 2], (size_t)r[i * 2 + 1], 0);
    }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindStorageBuffers stage=%d first=%d count=%d\n",
                stage, first, n);
}

void nvnCommandBufferBindTexture(cb_t *c, int stage, int i, uint64_t handle) {
    nvn_api("nvnCommandBufferBindTexture");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindTexture", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 128) { nvn_fail("nvnCommandBufferBindTexture", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    const tex_pool_t *bind_pool = nvn_tex_pool_for(c);
    const tex_t *bind_tex = (handle && bind_pool)
                          ? nvn_tex_pool_lookup(bind_pool, (int)(handle & 0xfffff)) : NULL;
    if (b) {
        b->tex[stage][i] = handle;
        if (handle) b->tex_mask[stage][i / 32] |= 1u << (i % 32);
        else b->tex_mask[stage][i / 32] &= ~(1u << (i % 32));
        nvn_cb_desc_event(c, (uint32_t)stage, 2, (uint32_t)i, 0, 0, handle);
    }
    nvn_global_tex_set(stage, i, handle, bind_tex);
    {
        static int on = -1;
        if (on < 0) on = getenv("TOTK_ANDROID_TRACE_UI_FLOW") != NULL;
        if (on && bind_tex && bind_tex->d.width <= 512 && bind_tex->d.height <= 512 &&
            bind_tex->d.width >= 64) {
            static unsigned logged;
            if (logged++ < 240)
                fprintf(stderr, "[totk-ui] bind cb=%p thread=%#x cmd=%u rec=%d stage=%d"
                                " slot=%d tex=%p fmt=%#x %ux%u target=%d\n",
                        (void *)c, runtime_thread_current_handle(), c->ncmds,
                        c->recording, stage, i, (const void *)bind_tex,
                        bind_tex->d.format, bind_tex->d.width, bind_tex->d.height,
                        bind_tex->d.target);
        }
    }
    if (nvn_trace() || (nvn_trace_first() && nvn_tex_trace++ < 64)) {
        int tid = (int)(handle & 0xfffff), sid = (int)((handle >> 20) & 0xfff);
        const tex_pool_t *tpool = nvn_tex_pool_for(c);
        const samp_pool_t *spool = nvn_samp_pool_for(c);
        const tex_t *t = tpool ? nvn_tex_pool_lookup(tpool, tid) : NULL;
        const samp_t *s = spool ? nvn_samp_pool_lookup(spool, sid) : NULL;
        if (t)
            fprintf(stderr, "[totk-nvn] BindTexture cb=%p tpool=%p spool=%p stage=%d index=%d"
                            " handle=%#llx tex=%d(%p) samp=%d(%p) target=%d format=%#x"
                            " %ux%ux%u levels=%u samples=%u\n",
                    (void *)c, (const void *)c->tex_pool, (const void *)c->samp_pool,
                    stage, i, (unsigned long long)handle, tid, (const void *)t, sid, (const void *)s,
                    t->d.target, t->d.format, t->d.width, t->d.height, t->d.depth,
                    t->d.levels, t->d.samples);
        else
            fprintf(stderr, "[totk-nvn] BindTexture cb=%p tpool=%p spool=%p stage=%d index=%d"
                            " handle=%#llx tex=%d(%p) samp=%d(%p) texture-not-in-pool\n",
                    (void *)c, (const void *)c->tex_pool, (const void *)c->samp_pool,
                    stage, i, (unsigned long long)handle, tid, (const void *)t, sid, (const void *)s);
    }
    if (nvn_trace_frame()) {
        int tid = (int)(handle & 0xfffff);
        const tex_pool_t *tp = nvn_tex_pool_for(c);
        const tex_t *t = tp ? nvn_tex_pool_lookup(tp, tid) : NULL;
        if (t && (t->d.format == 0x3f || t->d.width >= 1024)) {
            static unsigned n;
            if (n++ < 32)
                fprintf(stderr, "[totk-frame] bind tex cb=%p thread=%#x stage=%d index=%d handle=%#llx"
                                " tex=%p mem=%p off=%lld fmt=%#x %ux%u target=%d levels=%u\n",
                        (void *)c, runtime_thread_current_handle(), stage, i,
                        (unsigned long long)handle, (const void *)t, (void *)t->mem,
                        (long long)t->off, t->d.format, t->d.width,
                        t->d.height, t->d.target, t->d.levels);
        }
    }
}

void nvnCommandBufferBindTextures(cb_t *c, int stage, int first, int n, const uint64_t *handles) {
    nvn_api("nvnCommandBufferBindTextures");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindTextures", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || first < 0 || n < 0 || first + n > 128 || (!handles && n)) { nvn_fail("nvnCommandBufferBindTextures", "invalid handles"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) {
        for (int i = 0; i < n; i++) {
            int index = first + i;
            b->tex[stage][index] = handles[i];
            if (handles[i]) b->tex_mask[stage][index / 32] |= 1u << (index % 32);
            else b->tex_mask[stage][index / 32] &= ~(1u << (index % 32));
            nvn_cb_desc_event(c, (uint32_t)stage, 2, (uint32_t)index, 0, 0, handles[i]);
            const tex_pool_t *bp = nvn_tex_pool_for(c);
            const tex_t *bt = (handles[i] && bp)
                            ? nvn_tex_pool_lookup(bp, (int)(handles[i] & 0xfffff)) : NULL;
            nvn_global_tex_set(stage, index, handles[i], bt);
            static int on = -1;
            if (on < 0) on = getenv("TOTK_ANDROID_TRACE_UI_FLOW") != NULL;
            if (on && bt && bt->d.width <= 512 && bt->d.height <= 512 &&
                bt->d.width >= 64) {
                static unsigned logged;
                if (logged++ < 240)
                    fprintf(stderr, "[totk-ui] bind cb=%p thread=%#x cmd=%u rec=%d stage=%d"
                                    " slot=%d tex=%p fmt=%#x %ux%u target=%d\n",
                            (void *)c, runtime_thread_current_handle(), c->ncmds,
                            c->recording, stage, index, (const void *)bt,
                            bt->d.format, bt->d.width, bt->d.height, bt->d.target);
            }
        }
    }
    if (nvn_trace()) {
        for (int i = 0; i < n; i++)
            fprintf(stderr, "[totk-nvn] BindTextures stage=%d index=%d handle=%#llx\n",
                    stage, first + i, (unsigned long long)handles[i]);
    }
}

void nvnCommandBufferBindImage(cb_t *c, int stage, int i, uint64_t handle) {
    nvn_api("nvnCommandBufferBindImage");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindImage", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 16) { nvn_fail("nvnCommandBufferBindImage", "invalid binding"); return; }
    /* Same handle convention as BindTexture: the pool id lives in the low
     * bits (nvnDeviceGetImageHandle returns the index). */
    const tex_pool_t *pool = nvn_tex_pool_for(c);
    const tex_t *img = (handle && pool)
                     ? nvn_tex_pool_lookup(pool, (int)(handle & 0xfffff)) : NULL;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) {
        b->img[stage][i] = handle;
        if (handle) b->img_mask[stage] |= 1u << i;
        else b->img_mask[stage] &= ~(1u << i);
        nvn_cb_desc_event(c, (uint32_t)stage, 3, (uint32_t)i, 0, 0, handle);
    }
    nvn_global_img_set(stage, i, handle, img);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindImage stage=%d index=%d handle=%#llx img=%p\n",
                stage, i, (unsigned long long)handle, (const void *)img);
}

void nvnCommandBufferBindImages(cb_t *c, int stage, int first, int n, const uint64_t *handles) {
    nvn_api("nvnCommandBufferBindImages");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindImages", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || first < 0 || n < 0 || first + n > 16 || (!handles && n)) { nvn_fail("nvnCommandBufferBindImages", "invalid handles"); return; }
    for (int i = 0; i < n; i++)
        nvnCommandBufferBindImage(c, stage, first + i, handles[i]);
}

/* Replay recorded uniform updates in command order.  Draws are translated
 * after this, so every draw sees the final content of each range; the engine
 * gives each draw its own ring range, so this matches the GPU order. */
void nvn_cb_apply_uniform_updates(cb_t *cb) {
    if (!cb) return;
    for (uint32_t i = 0; i < cb->ncmds; i++) {
        nvn_cmd_t *c = &cb->cmds[i];
        if (c->kind != NVN_CMD_UPDATE_UBO || !c->u.update.data || !c->u.update.dst) continue;
        memcpy((void *)(uintptr_t)c->u.update.dst, c->u.update.data, c->u.update.size);
    }
}

/* nvnCommandBufferClearTexture: record the texture object, the view's
 * level/layer, the texel region, the colour and the channel mask.
 * `integer` marks the ClearTexturei/ui forms, whose colour is raw int/uint
 * bits (VkClearColorValue is a union, so a whole-level clear is exact). */
void nvn_cb_clear_texture(cb_t *c, void *t, void *v, void *r,
                                 const float rgba[4], int mask, int integer,
                                 const char *what) {
    nvn_api(what);
    nvn_cb_clear_texture_calls++;
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite clear texture t=%p mask=%d\n", t, mask);
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail(what, "invalid command buffer");
        return;
    }
    if (!nvn_tex_registry_find(t)) {
        nvn_fail(what, "invalid destination texture");
        return;
    }
    const int32_t *reg = r;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_CLEAR_TEXTURE);
    if (!cmd) return;
    cmd->u.tex_clear.tex = t;
    nvn_view_level_layer(v, &cmd->u.tex_clear.level, &cmd->u.tex_clear.layer);
    cmd->u.tex_clear.x = reg ? reg[0] : 0;
    cmd->u.tex_clear.y = reg ? reg[1] : 0;
    cmd->u.tex_clear.width = reg ? (uint32_t)(reg[3] > 0 ? reg[3] : 0) : 0;
    cmd->u.tex_clear.height = reg ? (uint32_t)(reg[4] > 0 ? reg[4] : 0) : 0;
    if (rgba) memcpy(cmd->u.tex_clear.rgba, rgba, sizeof cmd->u.tex_clear.rgba);
    cmd->u.tex_clear.mask = (uint32_t)mask;
    cmd->u.tex_clear.integer = (uint32_t)integer;
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] clear-texture cb=%p tex=%p region=%d,%d %ux%u"
                        " rgba=%.3f,%.3f,%.3f,%.3f mask=%#x int=%d\n", (void *)c, t,
                cmd->u.tex_clear.x, cmd->u.tex_clear.y, cmd->u.tex_clear.width,
                cmd->u.tex_clear.height, cmd->u.tex_clear.rgba[0],
                cmd->u.tex_clear.rgba[1], cmd->u.tex_clear.rgba[2],
                cmd->u.tex_clear.rgba[3], cmd->u.tex_clear.mask, integer);
}
