/* NVN module: command_buffer_0. */
#include "nvn_internal.h"

void nvn_mask_trace(const tex_t *t, const char *what) {
    if (!t) return;
    int mask = ((t->d.format == 0x1 || t->d.format == 0x3d) &&
                ((t->d.width == 2048 && t->d.height == 4096) ||
                 (t->d.width == 780 && t->d.height == 60)));
    if (!mask) return;
    tex_bridge_t *b = nvn_tex_bridge(t, 0);
    int rt = b && b->rt_valid;
    if (!strcmp(what, "bind")) g_mask_binds++;
    if (rt) g_mask_rt++;
    static unsigned logs;
    if (logs++ < 24 || nvn_trace_frame())
        fprintf(stderr, "[totk-nvn] mask %s tex=%p %ux%u rt=%d gen=%llu/%llu"
                        " gfx=%p dirty=%d binds=%u rt_binds=%u\n", what,
                (const void *)t, t->d.width, t->d.height, rt,
                b ? (unsigned long long)b->rt_frame : 0,
                b ? (unsigned long long)b->rt_uploaded : 0,
                b ? b->gfx : NULL, b ? b->gfx_dirty : 0, g_mask_binds, g_mask_rt);
}

cb_meta_t *nvn_cb_meta(const cb_t *c, int create) {
    cb_meta_t *free_slot = NULL;
    if (!c) return NULL;
    pthread_mutex_lock(&g_cb_meta_lock);
    for (unsigned i = 0; i < NVN_CB_META_MAX; i++) {
        if (g_cb_meta[i].cb == c) { pthread_mutex_unlock(&g_cb_meta_lock); return &g_cb_meta[i]; }
        if (!free_slot && !g_cb_meta[i].cb) free_slot = &g_cb_meta[i];
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->cb = c;
        pthread_mutex_unlock(&g_cb_meta_lock);
        return free_slot;
    }
    pthread_mutex_unlock(&g_cb_meta_lock);
    return NULL;
}

/* Does this recording contain anything that could produce a mask? */
int nvn_cb_mask_candidate(const cb_t *c, unsigned *kinds) {
    int found = 0;
    if (kinds) *kinds = 0;
    if (!c) return 0;
    for (uint32_t i = 0; i < c->ncmds; i++) {
        const nvn_cmd_t *cc = &c->cmds[i];
        if (cc->kind == NVN_CMD_COPY_BUFFER_TO_TEXTURE ||
            cc->kind == NVN_CMD_CLEAR_TEXTURE ||
            cc->kind == NVN_CMD_COPY_TEXTURE_TO_TEXTURE ||
            cc->kind == NVN_CMD_DISPATCH) {
            found = 1;
            if (kinds) *kinds |= 1u << (cc->kind & 15);
        } else if (cc->kind == NVN_CMD_SET_RENDER_TARGETS) {
            for (uint32_t k = 0; k < cc->u.targets.count; k++) {
                const tex_t *t = nvn_tex_registry_find(cc->u.targets.tex[k]);
                if (t && t->d.format == 0x1 &&
                    ((t->d.width == 2048 && t->d.height == 4096) ||
                     (t->d.width == 780 && t->d.height == 60))) {
                    found = 1;
                    if (kinds) *kinds |= 1u << 15;
                }
            }
        }
    }
    return found;
}

nvn_cb_target_t *nvn_cb_target(const cb_t *cb, int create) {
    nvn_cb_target_t *free_slot = NULL;
    if (!cb) return NULL;
    pthread_mutex_lock(&g_cb_target_lock);
    for (unsigned i = 0; i < sizeof g_cb_targets / sizeof g_cb_targets[0]; i++) {
        if (g_cb_targets[i].cb == cb) { pthread_mutex_unlock(&g_cb_target_lock); return &g_cb_targets[i]; }
        if (!free_slot && !g_cb_targets[i].cb) free_slot = &g_cb_targets[i];
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->cb = cb;
        pthread_mutex_unlock(&g_cb_target_lock);
        return free_slot;
    }
    pthread_mutex_unlock(&g_cb_target_lock);
    return NULL;
}

void nvn_cb_target_forget(const cb_t *cb) {
    nvn_cb_target_t *slot = nvn_cb_target(cb, 0);
    if (!slot) return;
    pthread_mutex_lock(&g_cb_target_lock);
    free(slot->draws);
    memset(slot, 0, sizeof *slot);
    pthread_mutex_unlock(&g_cb_target_lock);
}

/* Snapshot the state that is live at this draw. */
void nvn_cb_draw_snapshot(cb_t *c) {
    if (!c) return;
    nvn_cb_target_t *t = nvn_cb_target(c, 1);
    if (!t) return;
    if (t->ndraws == t->draws_cap) {
        uint32_t cap = t->draws_cap ? t->draws_cap * 2 : 8;
        if (cap > 512) return;
        nvn_draw_snap_t *n = realloc(t->draws, (size_t)cap * sizeof *n);
        if (!n) return;
        t->draws = n;
        t->draws_cap = cap;
    }
    nvn_draw_snap_t *d = &t->draws[t->ndraws++];
    memset(d, 0, sizeof *d);
    if (c->bind_state) {
        memcpy(&d->state, c->bind_state, sizeof d->state);
        d->have_state = 1;
    }
    d->layout = t->layout;
    d->width = t->width;
    d->height = t->height;
    d->have_layout = t->valid;
    d->window_texture = t->window_texture;
    d->color0_tex = t->color0_tex;
    d->cmd_index = c->ncmds ? c->ncmds - 1 : 0;
    if (nvn_trace_save_ui()) {
        const nvn_bind_state_t *b = d->have_state ? &d->state : NULL;
        fprintf(stderr, "[totk-save-ui] draw cb=%p prog=%p tgt=%p %ux%u win=%d fmt=%#x"
                        " draws=%d va=%#x ubo=%#x/%#x tex=%#x/%#x\n",
                (void *)c, (void *)c->prog, (void *)d->color0_tex, d->width, d->height,
                d->window_texture, d->layout.color_formats[0], c->draw_count,
                b ? b->va_mask : 0, b ? b->ubo_mask[0] : 0, b ? b->ubo_mask[1] : 0,
                b ? b->tex_mask[0][0] : 0, b ? b->tex_mask[1][0] : 0);
    }
}

const nvn_draw_snap_t *nvn_cb_last_draw(const cb_t *cb) {
    nvn_cb_target_t *t = nvn_cb_target(cb, 0);
    if (!t || !t->ndraws) return NULL;
    return &t->draws[t->ndraws - 1];
}

const nvn_bind_state_t *nvn_cb_draw_state(const cb_t *cb) {
    const nvn_draw_snap_t *d = nvn_cb_last_draw(cb);
    if (d && d->have_state) return &d->state;
    return cb ? (const nvn_bind_state_t *)cb->bind_state : NULL;
}

/* Build the per-target segments for a recording.  Returns a heap array the
 * caller frees (or attaches to the retired entry). */
uint32_t nvn_cb_segments(const cb_t *c, nvn_seg_t **out) {
    if (out) *out = NULL;
    if (!c || !c->ncmds) return 0;
    nvn_seg_t *segs = calloc((size_t)c->ncmds + 1, sizeof *segs);
    if (!segs) return 0;
    uint32_t nseg = 0, start = 0;
    int started = 0;
    GfxRenderTargetLayout cur;
    memset(&cur, 0, sizeof cur);
    uint32_t cw = 0, ch = 0;
    int cvalid = 0, cwin = 0;
    const void *ctex = NULL;
    /* NVN keeps the resolved render targets across BeginRecording.  A UI
     * recording often starts drawing to the target set before BeginRecording,
     * then issues its first SetRenderTargets later; seed the prefix segment
     * with that live target instead of dropping every prefix draw. */
    {
        nvn_cb_target_t *seed = nvn_cb_target(c, 0);
        if (seed && seed->valid) {
            cur = seed->layout;
            cw = seed->width;
            ch = seed->height;
            cvalid = 1;
            cwin = seed->window_texture;
            ctex = seed->color0_tex;
        }
    }
    for (uint32_t i = 0; i < c->ncmds; i++) {
        if (c->cmds[i].kind != NVN_CMD_SET_RENDER_TARGETS) continue;
        if (started) {
            nvn_seg_t *s = &segs[nseg++];
            s->color0_tex = ctex;
            s->layout = cur;
            s->width = cw;
            s->height = ch;
            s->have_layout = cvalid;
            s->window_texture = cwin;
            nvn_seg_fill(c, s, start, i);
        }
        memset(&cur, 0, sizeof cur);
        cw = ch = 0;
        cvalid = cwin = 0;
        ctex = NULL;
        uint32_t n = c->cmds[i].u.targets.count;
        void *targets[GFX_MAX_COLOR_TARGETS];
        for (uint32_t k = 0; k < n && k < GFX_MAX_COLOR_TARGETS; k++)
            targets[k] = c->cmds[i].u.targets.tex[k];
        if (n > 0 && n <= GFX_MAX_COLOR_TARGETS) {
            const char *why = NULL;
            /* The recording captured the resolved (view-aware) layout; only
             * fall back to re-resolving when it has none. */
            if (c->cmds[i].u.targets.have_layout) {
                cur = c->cmds[i].u.targets.layout;
                cw = c->cmds[i].u.targets.width;
                ch = c->cmds[i].u.targets.height;
                cvalid = 1;
            } else if (nvn_rt_layout_from_targets(n, targets, NULL,
                                                  c->cmds[i].u.targets.depth, NULL,
                                                  &cur, &cw, &ch, &why)) {
                cvalid = 1;
            }
            ctex = targets[0];
            if (ctex && nvn_window_for_texture(ctex)) cwin = 1;
        }
        started = 1;
        /* Keep the pre-target prefix only in the first segment. */
        start = nseg == 0 ? 0 : i;
    }
    if (started) {
        nvn_seg_t *s = &segs[nseg++];
        s->color0_tex = ctex;
        s->layout = cur;
        s->width = cw;
        s->height = ch;
        s->have_layout = cvalid;
        s->window_texture = cwin;
        nvn_seg_fill(c, s, start, c->ncmds);
    }
    if (!nseg) { free(segs); return 0; }
    if (out) *out = segs;
    else free(segs);
    return nseg;
}

int nvn_cb_track(cb_t *c) {
    for (unsigned i = 0; i < g_ncbs; i++) if (g_cbs[i] == c) return 1;
    if (g_ncbs == g_cbs_cap) {
        unsigned cap = g_cbs_cap ? g_cbs_cap * 2 : 64;
        cb_t **n = realloc(g_cbs, (size_t)cap * sizeof *n);
        if (!n) return 0;
        g_cbs = n;
        g_cbs_cap = cap;
    }
    g_cbs[g_ncbs++] = c;
    return 1;
}

void nvn_cb_untrack(const cb_t *c) {
    for (unsigned i = 0; i < g_ncbs; i++) {
        if (g_cbs[i] != c) continue;
        g_cbs[i] = g_cbs[--g_ncbs];
        g_cbs[g_ncbs] = NULL;
        return;
    }
}

nvn_cmd_t *nvn_cb_cmd(cb_t *c, uint32_t kind) {
    if (!c) return NULL;
    if (c->ncmds == c->cmds_cap) {
        uint32_t cap = c->cmds_cap ? c->cmds_cap * 2 : 64;
        if (cap > 65536) { nvn_fail("nvnCommandBuffer", "command list too large"); return NULL; }
        nvn_cmd_t *n = realloc(c->cmds, (size_t)cap * sizeof *n);
        if (!n) { nvn_fail("nvnCommandBuffer", "out of memory for command list"); return NULL; }
        c->cmds = n;
        c->cmds_cap = cap;
    }
    nvn_cmd_t *cmd = &c->cmds[c->ncmds++];
    memset(cmd, 0, sizeof *cmd);
    cmd->kind = kind;
    return cmd;
}

nvn_bind_state_t *nvn_cb_bind_state(cb_t *c) {
    if (!c) return NULL;
    if (!c->bind_state) {
        c->bind_state = calloc(1, sizeof(nvn_bind_state_t));
        if (c->bind_state) nvn_render_state_defaults(&((nvn_bind_state_t *)c->bind_state)->rs);
    }
    return c->bind_state;
}

void nvn_cb_state_event(cb_t *c) {
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_BIND_STATE);
    if (cmd) cmd->u.state.rs = b->rs;
}

/* Descriptor events are recorded in command order so the backend can give
 * each draw its own descriptor epoch.  kind: 0 UBO, 1 SSBO, 2 texture,
 * 3 storage image. */
void nvn_cb_desc_event(cb_t *c, uint32_t stage, uint32_t kind, uint32_t slot,
                              uint64_t address, size_t size, uint64_t handle) {
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_BIND_DESC);
    if (!cmd) return;
    cmd->u.desc.stage = stage;
    cmd->u.desc.kind = kind;
    cmd->u.desc.slot = slot;
    cmd->u.desc.address = address;
    cmd->u.desc.size = size;
    cmd->u.desc.handle = handle;
}

void nvn_cb_free_commands(cb_t *c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->ncmds; i++)
        nvn_cmd_payload_free(&c->cmds[i]);
    free(c->cmds);
    c->cmds = NULL;
    c->ncmds = c->cmds_cap = 0;
}

int nvn_cmd_clone(nvn_cmd_t *dst, const nvn_cmd_t *src) {
    *dst = *src;
    switch (src->kind) {
    case NVN_CMD_UPDATE_UBO:
        dst->u.update.data = NULL;
        if (!src->u.update.data || !src->u.update.size) return 1;
        {
            uint8_t *copy = malloc(src->u.update.size);
            if (!copy) return 0;
            memcpy(copy, src->u.update.data, src->u.update.size);
            dst->u.update.data = copy;
        }
        return 1;
    case NVN_CMD_COPY_BUFFER_TO_TEXTURE:
        dst->u.buf_to_tex.data = NULL;
        dst->u.buf_to_tex.data_size = 0;
        if (!src->u.buf_to_tex.data || !src->u.buf_to_tex.data_size) return 1;
        {
            uint8_t *copy = malloc(src->u.buf_to_tex.data_size);
            if (!copy) return 0;
            memcpy(copy, src->u.buf_to_tex.data, src->u.buf_to_tex.data_size);
            dst->u.buf_to_tex.data = copy;
            dst->u.buf_to_tex.data_size = src->u.buf_to_tex.data_size;
        }
        return 1;
    case NVN_CMD_DRAW:
        memset(dst->u.draw.vb_data, 0, sizeof dst->u.draw.vb_data);
        memset(dst->u.draw.vb_size, 0, sizeof dst->u.draw.vb_size);
        dst->u.draw.vb_count = 0;
        for (uint32_t k = 0; k < src->u.draw.vb_count && k < 4; k++) {
            if (!src->u.draw.vb_data[k] || !src->u.draw.vb_size[k]) continue;
            if (!nvn_vb_snap_reserve(src->u.draw.vb_size[k])) continue;
            uint8_t *copy = malloc(src->u.draw.vb_size[k]);
            if (!copy) { nvn_vb_snap_release(src->u.draw.vb_size[k]); continue; }
            memcpy(copy, src->u.draw.vb_data[k], src->u.draw.vb_size[k]);
            dst->u.draw.vb_data[k] = copy;
            dst->u.draw.vb_size[k] = src->u.draw.vb_size[k];
            dst->u.draw.vb_stream[k] = src->u.draw.vb_stream[k];
            dst->u.draw.vb_count = k + 1;
        }
        return 1;
    case NVN_CMD_DRAW_INDEXED:
        dst->u.draw_indexed.index_data = NULL;
        dst->u.draw_indexed.index_size = 0;
        memset(dst->u.draw_indexed.vb_data, 0, sizeof dst->u.draw_indexed.vb_data);
        memset(dst->u.draw_indexed.vb_size, 0, sizeof dst->u.draw_indexed.vb_size);
        dst->u.draw_indexed.vb_count = 0;
        for (uint32_t k = 0; k < src->u.draw_indexed.vb_count && k < 4; k++) {
            if (!src->u.draw_indexed.vb_data[k] || !src->u.draw_indexed.vb_size[k]) continue;
            if (!nvn_vb_snap_reserve(src->u.draw_indexed.vb_size[k])) continue;
            uint8_t *copy = malloc(src->u.draw_indexed.vb_size[k]);
            if (!copy) { nvn_vb_snap_release(src->u.draw_indexed.vb_size[k]); continue; }
            memcpy(copy, src->u.draw_indexed.vb_data[k], src->u.draw_indexed.vb_size[k]);
            dst->u.draw_indexed.vb_data[k] = copy;
            dst->u.draw_indexed.vb_size[k] = src->u.draw_indexed.vb_size[k];
            dst->u.draw_indexed.vb_stream[k] = src->u.draw_indexed.vb_stream[k];
            dst->u.draw_indexed.vb_count = k + 1;
        }
        if (!src->u.draw_indexed.index_data || !src->u.draw_indexed.index_size) return 1;
        {
            uint8_t *copy = malloc(src->u.draw_indexed.index_size);
            if (!copy) { nvn_cb_snapshot_free(dst); return 0; }
            memcpy(copy, src->u.draw_indexed.index_data, src->u.draw_indexed.index_size);
            dst->u.draw_indexed.index_data = copy;
            dst->u.draw_indexed.index_size = src->u.draw_indexed.index_size;
        }
        return 1;
    case NVN_CMD_DISPATCH:
        /* Each copy owns its descriptor snapshot and its own reference on the
         * program state, exactly like a fresh recording. */
        dst->u.dispatch.bind = NULL;
        dst->u.dispatch.pstate = NULL;
        if (src->u.dispatch.bind) {
            nvn_dispatch_snap_t *b = malloc(sizeof *b);
            if (!b) return 0;
            memcpy(b, src->u.dispatch.bind, sizeof *b);
            dst->u.dispatch.bind = b;
        }
        if (src->u.dispatch.pstate) {
            nvn_prog_ref(src->u.dispatch.pstate);
            dst->u.dispatch.pstate = src->u.dispatch.pstate;
        }
        return 1;
    default:
        return 1;
    }
}

/* Release the heap payload of one recorded command.  Used by both the live
 * command list and the retired deep copies. */
void nvn_cmd_payload_free(nvn_cmd_t *cmd) {
    if (!cmd) return;
    switch (cmd->kind) {
    case NVN_CMD_UPDATE_UBO:
        free(cmd->u.update.data);
        cmd->u.update.data = NULL;
        break;
    case NVN_CMD_COPY_BUFFER_TO_TEXTURE:
        free(cmd->u.buf_to_tex.data);
        cmd->u.buf_to_tex.data = NULL;
        cmd->u.buf_to_tex.data_size = 0;
        break;
    case NVN_CMD_DRAW:
        nvn_cb_snapshot_free(cmd);
        break;
    case NVN_CMD_DRAW_INDEXED:
        nvn_cb_snapshot_free(cmd);
        free(cmd->u.draw_indexed.index_data);
        cmd->u.draw_indexed.index_data = NULL;
        cmd->u.draw_indexed.index_size = 0;
        break;
    case NVN_CMD_DISPATCH:
        free(cmd->u.dispatch.bind);
        cmd->u.dispatch.bind = NULL;
        if (cmd->u.dispatch.pstate) {
            nvn_prog_release(cmd->u.dispatch.pstate);
            cmd->u.dispatch.pstate = NULL;
        }
        break;
    default:
        break;
    }
}
