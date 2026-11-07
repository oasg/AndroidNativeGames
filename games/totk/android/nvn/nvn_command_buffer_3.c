/* NVN module: command_buffer_3. */
#include "nvn_internal.h"

void nvnCommandBufferBindVertexBuffer(cb_t *c, int index, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindVertexBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindVertexBuffer", "invalid command buffer"); return; }
    if (index < 0 || index >= 16 || !address || !size) { nvn_fail("nvnCommandBufferBindVertexBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    b->vb[index].address = address;
    b->vb[index].size = size;
    b->vb_mask |= 1u << index;
    if (nvn_trace() || (nvn_trace_first() && nvn_vtx_trace++ < 8192))
        fprintf(stderr, "[totk-nvn] BindVertexBuffer cb=%p rec=%u index=%d addr=%#llx size=%zu\n",
                (void *)c, c->recording, index, (unsigned long long)address, size);
}

void nvnCommandBufferBindVertexBuffers(cb_t *c, int first, int n, const void *ranges) {
    nvn_api("nvnCommandBufferBindVertexBuffers");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindVertexBuffers", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || first + n > 16 || (!ranges && n)) { nvn_fail("nvnCommandBufferBindVertexBuffers", "invalid range"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    const uint64_t *r = ranges;
    for (int i = 0; i < n; i++) {
        b->vb[first + i].address = r[i * 2];
        b->vb[first + i].size = (size_t)r[i * 2 + 1];
        b->vb_mask |= 1u << (first + i);
    }
    if (nvn_trace() || (nvn_trace_first() && nvn_vtx_trace++ < 8192))
        fprintf(stderr, "[totk-nvn] BindVertexBuffers cb=%p rec=%u first=%d count=%d\n",
                (void *)c, c->recording, first, n);
}

void nvnCommandBufferBindVertexAttribState(cb_t *c, int n, const void *states) {
    nvn_api("nvnCommandBufferBindVertexAttribState");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindVertexAttribState", "invalid command buffer"); return; }
    if (n < 0 || n > 16 || (!states && n)) { nvn_fail("nvnCommandBufferBindVertexAttribState", "invalid attribute count"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    for (int i = 0; i < 16; i++) {
        if (i < n) memcpy(&b->va[i], (const uint8_t *)states + (size_t)i * 4, 4);
        else b->va[i] = 0;
        if (i < n) b->va_mask |= 1u << i;
        else b->va_mask &= ~(1u << i);
    }
    if (nvn_trace() || (nvn_trace_first() && nvn_vtx_trace++ < 8192)) {
        fprintf(stderr, "[totk-nvn] BindVertexAttribState cb=%p rec=%u count=%d",
                (void *)c, c->recording, n);
        for (int i = 0; i < n; i++) fprintf(stderr, " [%d]=%#x", i, b->va[i]);
        fprintf(stderr, "\n");
    }
}

void nvnCommandBufferBindVertexStreamState(cb_t *c, int n, const void *states) {
    nvn_api("nvnCommandBufferBindVertexStreamState");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindVertexStreamState", "invalid command buffer"); return; }
    if (n < 0 || n > 16 || (!states && n)) { nvn_fail("nvnCommandBufferBindVertexStreamState", "invalid stream count"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    for (int i = 0; i < 16; i++) {
        if (i < n) memcpy(&b->vs[i], (const uint8_t *)states + (size_t)i * 8, 8);
        else b->vs[i] = 0;
        if (i < n) b->vs_mask |= 1u << i;
        else b->vs_mask &= ~(1u << i);
    }
    if (nvn_trace() || (nvn_trace_first() && nvn_vtx_trace++ < 8192)) {
        fprintf(stderr, "[totk-nvn] BindVertexStreamState cb=%p rec=%u count=%d",
                (void *)c, c->recording, n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, " [%d]=%#llx", i, (unsigned long long)b->vs[i]);
        fprintf(stderr, "\n");
    }
}

/* Buffer copy/clear execute on the host memory the guest handed us.  The
 * backend is synchronous, so a CPU copy at record time matches the order the
 * guest asked for and makes GPU-staged constant/style buffers visible to the
 * draws that follow. */
void nvnCommandBufferCopyBufferToBuffer(cb_t *c, uint64_t src, uint64_t dst,
                                               size_t n, int flags) {
    nvn_api("nvnCommandBufferCopyBufferToBuffer");
    nvn_cb_copy_b2b++;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferCopyBufferToBuffer", "invalid command buffer"); return; }
    (void)flags;
    if (!src || !dst || !n) { nvn_fail("nvnCommandBufferCopyBufferToBuffer", "null range"); return; }
    if (n > (256u << 20)) { nvn_fail("nvnCommandBufferCopyBufferToBuffer", "range too large"); return; }
    if (src != dst) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
    if (nvn_trace_first() && nvn_copy_trace++ < 32)
        fprintf(stderr, "[totk-nvn] CopyBufferToBuffer cb=%p src=%#llx dst=%#llx n=%zu\n",
                (void *)c, (unsigned long long)src, (unsigned long long)dst, n);
}

void nvnCommandBufferClearBuffer(cb_t *c, uint64_t dst, size_t n, uint32_t value) {
    nvn_api("nvnCommandBufferClearBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferClearBuffer", "invalid command buffer"); return; }
    if (!dst || !n) { nvn_fail("nvnCommandBufferClearBuffer", "null range"); return; }
    if (n > (256u << 20)) { nvn_fail("nvnCommandBufferClearBuffer", "range too large"); return; }
    memset((void *)(uintptr_t)dst, (int)(value & 0xffu), n);
    if (nvn_trace_first() && nvn_copy_trace++ < 32)
        fprintf(stderr, "[totk-nvn] ClearBuffer cb=%p dst=%#llx n=%zu value=%#x\n",
                (void *)c, (unsigned long long)dst, n, value);
}

cb_t *nvn_cb_by_handle(uint64_t h) {
    for (unsigned i = 0; i < g_ncbs; i++) {
        cb_t *c = g_cbs[i];
        if (c && c->magic == NVN_MAGIC('C', 'M', 'D', 'B') && c->handle == h) return c;
    }
    return NULL;
}

int nvn_cb_append_command(cb_t *dst, const nvn_cmd_t *src) {
    nvn_cmd_t *d = nvn_cb_cmd(dst, src->kind);
    if (!d) return 0;
    return nvn_cmd_clone(d, src);
}

void nvn_cb_merge_commands(cb_t *dst, const cb_t *src, const char *what) {
    if (!dst || !src) return;
    for (uint32_t i = 0; i < src->ncmds; i++)
        if (!nvn_cb_append_command(dst, &src->cmds[i])) break;
    nvn_bind_state_t *d = nvn_cb_bind_state(dst);
    const nvn_bind_state_t *s = src->bind_state;
    nvn_bind_state_merge(d, s);
    if (src->prog) dst->prog = src->prog;
    if (src->win) dst->win = src->win;
    if (src->tex_pool) dst->tex_pool = src->tex_pool;
    if (src->samp_pool) dst->samp_pool = src->samp_pool;
    dst->draw_count += src->draw_count;
    nvn_cb_target_t *st_t = nvn_cb_target(src, 0);
    nvn_cb_target_t *dt_t = nvn_cb_target(dst, 1);
    if (st_t && dt_t && st_t->valid) *dt_t = *st_t, dt_t->cb = dst;
    if (nvn_trace_first() || nvn_trace())
        fprintf(stderr, "[totk-nvn] %s dst=%p src=%p src_cmds=%u draw=%d"
                        " ubo=%#x va=%#x tex=%#x\n", what, (void *)dst, (void *)src,
                src->ncmds, src->draw_count,
                s ? s->ubo_mask[0] : 0, s ? s->va_mask : 0, s ? s->tex_mask[0][0] : 0);
}

/* Replay a finalized recording from the retired table.  The lock is held while
 * copying so an entry cannot be evicted in the middle of the merge. */
int nvn_cb_merge_retired(cb_t *dst, uint64_t handle, const char *what) {
    if (!dst) return 0;
    pthread_mutex_lock(&g_retired_lock);
    nvn_retired_t *e = NULL;
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++)
        if (g_retired[i].used && g_retired[i].handle == handle) { e = &g_retired[i]; break; }
    if (!e) { pthread_mutex_unlock(&g_retired_lock); return 0; }
    uint32_t appended = 0;
    for (uint32_t i = 0; i < e->ncmds; i++) {
        if (!nvn_cb_append_command(dst, &e->cmds[i])) break;
        appended++;
    }
    nvn_bind_state_t *d = nvn_cb_bind_state(dst);
    nvn_bind_state_merge(d, e->bind);
    if (e->prog) dst->prog = e->prog;
    if (e->win) dst->win = e->win;
    if (e->tex_pool) dst->tex_pool = e->tex_pool;
    if (e->samp_pool) dst->samp_pool = e->samp_pool;
    dst->draw_count += (int)e->draw_count;
    if (e->have_layout) {
        nvn_cb_target_t *dt = nvn_cb_target(dst, 1);
        if (dt) {
            dt->layout = e->layout;
            dt->width = e->width;
            dt->height = e->height;
            dt->valid = 1;
            dt->window_texture = e->window_texture;
        }
    }
    e->age = ++g_retired_age;   /* keep a handle the engine replays hot */
    uint32_t ncmds = e->ncmds, draws = e->draw_count;
    int has_bind = e->bind != NULL;
    pthread_mutex_unlock(&g_retired_lock);
    if (nvn_trace_composite())
        fprintf(stderr, "[totk-frame] composite merge %s handle=%#llx dst=%p ncmds=%u"
                        " draws=%u appended=%u snapshots=lost\n", what,
                (unsigned long long)handle, (void *)dst, ncmds, draws, appended);
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] %s retired handle=%#llx dst=%p ncmds=%u draws=%u"
                        " appended=%u bind=%d\n", what, (unsigned long long)handle,
                (void *)dst, ncmds, draws, appended, has_bind);
    return appended != 0 || draws != 0;
}

void nvnCommandBufferCallCommands(cb_t *c, int n, const uint64_t *handles) {
    nvn_api("nvnCommandBufferCallCommands");
    nvn_cbacct_bump(4);
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferCallCommands", "invalid command buffer"); return; }
    if (n < 0 || (!handles && n)) { nvn_fail("nvnCommandBufferCallCommands", "null handle array"); return; }
    {
        static int on = -1;
        if (on < 0) on = getenv("TOTK_ANDROID_TRACE_CB_FLOW") != NULL;
        if (on) {
            static unsigned shown;
            if (shown++ < 240) {
                fprintf(stderr, "[totk-cb] call dst=%p thread=%#x n=%d", (void *)c,
                        runtime_thread_current_handle(), n);
                for (int i = 0; i < n; i++) fprintf(stderr, " %#llx", (unsigned long long)handles[i]);
                fputc('\n', stderr);
            }
        }
    }
    if (nvn_trace_frame()) {
        fprintf(stderr, "[totk-frame] CallCommands dst=%p thread=%#x n=%d", (void *)c,
                runtime_thread_current_handle(), n);
        for (int i = 0; i < n; i++) {
            cb_t *src = nvn_cb_by_handle(handles[i]);
            fprintf(stderr, " %#llx%s%s", (unsigned long long)handles[i],
                    src ? "" : "(dead)", src && src->ncmds ? "(cmds)" : "");
        }
        fputc('\n', stderr);
    }
    for (int i = 0; i < n; i++) {
        cb_t *src = nvn_cb_by_handle(handles[i]);
        if (src) {
            nvn_cb_merge_commands(c, src, "CallCommands");
            continue;
        }
        if (!nvn_cb_merge_retired(c, handles[i], "CallCommands") && nvn_trace())
            fprintf(stderr, "[totk-nvn] CallCommands handle %#llx not live or retired\n",
                    (unsigned long long)handles[i]);
    }
}

void nvnCommandBufferCopyCommands(cb_t *c, int n, const uint64_t *handles) {
    nvn_api("nvnCommandBufferCopyCommands");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferCopyCommands", "invalid command buffer"); return; }
    if (n < 0 || (!handles && n)) { nvn_fail("nvnCommandBufferCopyCommands", "null handle array"); return; }
    if (nvn_trace_frame()) {
        fprintf(stderr, "[totk-frame] CopyCommands dst=%p thread=%#x n=%d", (void *)c,
                runtime_thread_current_handle(), n);
        for (int i = 0; i < n; i++) {
            cb_t *src = nvn_cb_by_handle(handles[i]);
            fprintf(stderr, " %#llx%s%s", (unsigned long long)handles[i],
                    src ? "" : "(dead)", src && src->ncmds ? "(cmds)" : "");
        }
        fputc('\n', stderr);
    }
    for (int i = 0; i < n; i++) {
        cb_t *src = nvn_cb_by_handle(handles[i]);
        if (src) {
            nvn_cb_merge_commands(c, src, "CopyCommands");
            continue;
        }
        if (!nvn_cb_merge_retired(c, handles[i], "CopyCommands") && nvn_trace())
            fprintf(stderr, "[totk-nvn] CopyCommands handle %#llx not live or retired\n",
                    (unsigned long long)handles[i]);
    }
}

void nvnCommandBufferSetPrimitiveRestart(cb_t *c, uint32_t enable, int index) {    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetPrimitiveRestart", "invalid command buffer"); return; }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] SetPrimitiveRestart enable=%u index=%d\n", enable, index);
}

/* Deep-copy the index bytes of a recorded DrawElements.  The guest reuses its
 * index ring buffer between recording and the queued segment replay, so the
 * address alone is not enough for a deferred draw. */
void nvn_cb_snapshot_indices(nvn_cmd_t *cmd, int type, int count, uint64_t index) {
    if (!cmd || !index || count <= 0) return;
    size_t isz = type == 0 ? 1u : type == 1u ? 2u : 4u;
    size_t n = (size_t)count * isz;
    if (n > (64u << 20)) return;
    uint8_t *copy = malloc(n);
    if (!copy) return;
    memcpy(copy, (const void *)(uintptr_t)index, n);
    cmd->u.draw_indexed.index_data = copy;
    cmd->u.draw_indexed.index_size = n;
}

int nvn_vb_snap_reserve(size_t n) {
    int ok = 0;
    pthread_mutex_lock(&g_vb_snap_lock);
    if (g_vb_snap_live + n <= NVN_VB_SNAP_TOTAL) {
        g_vb_snap_live += n;
        ok = 1;
    }
    pthread_mutex_unlock(&g_vb_snap_lock);
    return ok;
}

void nvn_vb_snap_release(size_t n) {
    pthread_mutex_lock(&g_vb_snap_lock);
    if (g_vb_snap_live >= n) g_vb_snap_live -= n;
    else g_vb_snap_live = 0;
    pthread_mutex_unlock(&g_vb_snap_lock);
}

/* Vertex count an indexed command references, so the snapshot covers the
 * bytes the draw really reads even when the guest declared a smaller range. */
uint32_t nvn_cb_index_vertex_count(const nvn_cmd_t *cmd) {
    if (!cmd || cmd->kind != NVN_CMD_DRAW_INDEXED) return 0;
    const uint8_t *idx = cmd->u.draw_indexed.index_data;
    uint32_t n = cmd->u.draw_indexed.count;
    uint32_t maxi = 0;
    if (!idx || !n) return cmd->u.draw_indexed.base_vertex + n;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t v;
        if (cmd->u.draw_indexed.index_type == 0) v = idx[k];
        else if (cmd->u.draw_indexed.index_type == 1) {
            uint16_t x = 0; memcpy(&x, idx + (size_t)k * 2, 2); v = x;
        } else {
            v = 0; memcpy(&v, idx + (size_t)k * 4, 4);
        }
        if (v > maxi) maxi = v;
    }
    return cmd->u.draw_indexed.base_vertex + maxi + 1;
}

void nvn_cb_snapshot_vertex_buffers(nvn_cmd_t *cmd, const nvn_bind_state_t *b,
                                           uint32_t need_verts) {
    if (!cmd || !b) return;
    uint8_t **data;
    size_t *size;
    uint8_t *stream;
    uint32_t *count;
    if (cmd->kind == NVN_CMD_DRAW) {
        data = cmd->u.draw.vb_data; size = cmd->u.draw.vb_size;
        stream = cmd->u.draw.vb_stream; count = &cmd->u.draw.vb_count;
    } else if (cmd->kind == NVN_CMD_DRAW_INDEXED) {
        data = cmd->u.draw_indexed.vb_data; size = cmd->u.draw_indexed.vb_size;
        stream = cmd->u.draw_indexed.vb_stream; count = &cmd->u.draw_indexed.vb_count;
    } else {
        return;
    }
    uint32_t n = 0;
    for (int i = 0; i < 16 && n < 4; i++) {
        if (!(b->vb_mask & (1u << i)) || !b->vb[i].address || !b->vb[i].size)
            continue;
        /* A huge scene stream is not recyclable within a frame's reach and a
         * private copy would be pointless; leave it to the guest address. */
        if (b->vb[i].size > NVN_VB_SNAP_CAP) continue;
        uint32_t stride = b->vs[i] ? (uint32_t)b->vs[i] : 1;
        size_t want = b->vb[i].size;
        size_t needed = (size_t)need_verts * stride;
        if (needed > want) {
            if (needed > NVN_VB_SNAP_CAP) needed = NVN_VB_SNAP_CAP;
            /* Extend from the guest ring only when the range is mapped. */
            if (nvn_guest_range_ok(b->vb[i].address, needed)) want = needed;
        }
        if (!want || want > NVN_VB_SNAP_CAP) continue;
        if (!nvn_vb_snap_reserve(want)) continue;
        uint8_t *copy = malloc(want);
        if (!copy) { nvn_vb_snap_release(want); continue; }
        memcpy(copy, (const void *)(uintptr_t)b->vb[i].address, want);
        data[n] = copy;
        size[n] = want;
        stream[n] = (uint8_t)i;
        n++;
    }
    *count = n;
}

void nvn_cb_snapshot_free(nvn_cmd_t *cmd) {
    if (!cmd) return;
    uint8_t **data = NULL;
    size_t *size = NULL;
    uint32_t *count = NULL;
    if (cmd->kind == NVN_CMD_DRAW) {
        data = cmd->u.draw.vb_data; size = cmd->u.draw.vb_size;
        count = &cmd->u.draw.vb_count;
    } else if (cmd->kind == NVN_CMD_DRAW_INDEXED) {
        data = cmd->u.draw_indexed.vb_data; size = cmd->u.draw_indexed.vb_size;
        count = &cmd->u.draw_indexed.vb_count;
    }
    if (!data || !count) return;
    for (uint32_t i = 0; i < *count && i < 4; i++) {
        if (data[i]) {
            nvn_vb_snap_release(size[i]);
            free(data[i]);
            data[i] = NULL;
            size[i] = 0;
        }
    }
    *count = 0;
}

/* Record-time vertex-stream evidence for the large text/UI passes.  Replay
 * shows the address has been recycled; this prints the bytes while the guest
 * still owns them, so the two traces can be compared directly. */
int nvn_cb_trace_record_vb_want(cb_t *c) {
    if (!c || (!nvn_trace_inputs() && !nvn_trace_composite())) return 0;
    nvn_cb_target_t *t = nvn_cb_target(c, 0);
    return t && t->valid && (t->width >= 1024 || t->height >= 1024);
}

void nvn_cb_trace_record_vb(cb_t *c, const char *what, uint32_t count) {
    static unsigned seen, logged;
    if (!nvn_cb_trace_record_vb_want(c)) return;
    seen++;
    int want = seen <= 64 || (seen % 256) == 0;
    if (!want || logged >= 256) return;
    logged++;
    const nvn_bind_state_t *b = c->bind_state;
    nvn_cb_target_t *t = nvn_cb_target(c, 0);
    fprintf(stderr, "[totk-record-vb] #%u %s cb=%p %ux%u count=%u vb_mask=%#x",
            logged, what, (void *)c, t ? t->width : 0, t ? t->height : 0, count,
            b ? b->vb_mask : 0);
    if (b) for (int i = 0; i < 16; i++) {
        if (!(b->vb_mask & (1u << i)) || !b->vb[i].address) continue;
        uint32_t w[8] = { 0 };
        size_t nw = b->vb[i].size < sizeof w ? b->vb[i].size : sizeof w;
        /* This is the guest's own bound stream at draw-record time, so reading
         * it is as valid as the snapshot copy below; the pool-range check would
         * reject ring windows that are mapped but not fully inside one epoch. */
        if (nw) memcpy(w, (const void *)(uintptr_t)b->vb[i].address, nw);
        fprintf(stderr, " | vb%d addr=%#llx size=%zu first=%08x %08x %08x %08x"
                        " %08x %08x %08x %08x",
                i, (unsigned long long)b->vb[i].address, b->vb[i].size,
                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    fputc('\n', stderr);
}

void nvnCommandBufferDrawElements(cb_t *c, int mode, int type, int count,
                                         uint64_t index) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferDrawElements", "invalid command buffer"); return; }
    if (count < 0 || !index) { nvn_fail("nvnCommandBufferDrawElements", "invalid count/index"); return; }
    if (type < 0 || type > 2) { nvn_fail("nvnCommandBufferDrawElements", "invalid index type"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW_INDEXED);
    if (cmd) {
        cmd->u.draw_indexed.primitive = (uint32_t)mode;
        cmd->u.draw_indexed.count = (uint32_t)count;
        cmd->u.draw_indexed.index_type = (uint32_t)type;
        cmd->u.draw_indexed.base_vertex = 0;
        cmd->u.draw_indexed.indices = index;
        nvn_cb_snapshot_indices(cmd, type, count, index);
        nvn_cb_snapshot_vertex_buffers(cmd, c->bind_state,
                                       nvn_cb_index_vertex_count(cmd));
    }
    c->draw_count += count;
    nvn_cb_draw_snapshot(c);
    nvn_frame_draw_trace(c, "elements", count);
    nvn_cb_trace_record_vb(c, "elements", (uint32_t)count);
    if (nvn_trace() || (nvn_trace_first() && nvn_draw_trace++ < 32))
        fprintf(stderr, "[totk-nvn] DrawElements cb=%p rec=%u mode=%d type=%d count=%d index=%#llx"
                        " va_mask=%#x vs_mask=%#x\n", (void *)c, c->recording, mode, type, count,
                (unsigned long long)index,
                c->bind_state ? ((nvn_bind_state_t *)c->bind_state)->va_mask : 0,
                c->bind_state ? ((nvn_bind_state_t *)c->bind_state)->vs_mask : 0);
}
