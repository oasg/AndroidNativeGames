/* NVN module: scene. */
#include "nvn_internal.h"

static nvn_scene_item_t g_scene_pending[NVN_SCENE_QUEUE_MAX];

static unsigned g_scene_pending_count;

static unsigned nvn_scene_dropped;

static pthread_mutex_t g_scene_lock = PTHREAD_MUTEX_INITIALIZER;

void nvn_seg_fill(const cb_t *c, nvn_seg_t *s, uint32_t first, uint32_t end) {
    s->first_cmd = first;
    s->ncmds = end > first ? end - first : 0;
    for (uint32_t i = first; i < end && i < c->ncmds; i++) {
        const nvn_cmd_t *cc = &c->cmds[i];
        if (cc->kind == NVN_CMD_DRAW) s->draw_count += cc->u.draw.count;
        else if (cc->kind == NVN_CMD_DRAW_INDEXED) s->draw_count += cc->u.draw_indexed.count;
        else if (cc->kind == NVN_CMD_DISPATCH ||
                 cc->kind == NVN_CMD_CLEAR_TEXTURE ||
                 cc->kind == NVN_CMD_COPY_TEXTURE_TO_TEXTURE ||
                 cc->kind == NVN_CMD_COPY_BUFFER_TO_TEXTURE ||
                 cc->kind == NVN_CMD_COPY_TEXTURE_TO_BUFFER) s->gpu_count++;
        else if (cc->kind == NVN_CMD_CLEAR_COLOR && cc->u.clear.index == 0) {
            s->clear_count++;
            s->clear_mask = cc->u.clear.mask;
            memcpy(s->clear_rgba, cc->u.clear.rgba, sizeof s->clear_rgba);
        }
    }
    nvn_cb_target_t *t = nvn_cb_target(c, 0);
    if (!t || !t->draws) return;
    for (uint32_t i = 0; i < t->ndraws; i++) {
        const nvn_draw_snap_t *d = &t->draws[i];
        if (d->cmd_index < first || d->cmd_index >= end) continue;
        s->last = *d;
        s->have_last = 1;
    }
}

/* The title UI records ~100 targets per frame but only one segment per submit
 * used to drain.  A plain FIFO overflows and drops the 0x3f composite that the
 * window samples.  Segments with draws and sampled textures go to the front;
 * empty clear/state segments stay behind. */
void nvn_scene_queue(uint64_t handle, uint32_t seg, uint64_t seq, uint32_t prio,
                            const void *target, uint32_t width, uint32_t height,
                            uint32_t draws) {
    if (getenv("TOTK_ANDROID_NO_SCENE_QUEUE")) return;
    pthread_mutex_lock(&g_scene_lock);
    for (unsigned i = 0; i < g_scene_pending_count; i++)
        if (g_scene_pending[i].handle == handle && g_scene_pending[i].seg == seg) {
            if (prio > g_scene_pending[i].prio) g_scene_pending[i].prio = prio;
            pthread_mutex_unlock(&g_scene_lock);
            return;
        }
    /* A newer recording re-renders the same guest target, so an older
     * recording's entry for that target can never be sampled again.  Keeping
     * it only crowds out the producer/consumer chain that is due now. */
    for (unsigned i = 0; i < g_scene_pending_count; ) {
        if (g_scene_pending[i].target == target && g_scene_pending[i].handle != handle) {
            memmove(&g_scene_pending[i], &g_scene_pending[i + 1],
                    (g_scene_pending_count - i - 1) * sizeof g_scene_pending[0]);
            g_scene_pending_count--;
            continue;
        }
        i++;
    }
    if (g_scene_pending_count == NVN_SCENE_QUEUE_MAX) {
        /* FIFO: the head is the oldest and therefore the most stale entry. */
        if (nvn_ui_seg_want())
            fprintf(stderr, "[totk-ui-seg] queue full: drop handle=%#llx seg=%u prio=%u"
                            " depth=%u\n", (unsigned long long)g_scene_pending[0].handle,
                    g_scene_pending[0].seg, g_scene_pending[0].prio,
                    g_scene_pending_count);
        nvn_scene_dropped++;
        memmove(&g_scene_pending[0], &g_scene_pending[1],
                (NVN_SCENE_QUEUE_MAX - 1) * sizeof g_scene_pending[0]);
        g_scene_pending_count--;
    }
    /* Append: a recording's segments are queued in ascending segment order and
     * the drain consumes them from the head, so a producer always runs before
     * the consumers that sample its target.  Priority used to reorder the
     * queue; that starved the 60-draw prio-1 producer behind every prio-2
     * consumer, leaving the whole 360x69 chain on an untouched target. */
    unsigned at = g_scene_pending_count++;
    g_scene_pending[at].handle = handle;
    g_scene_pending[at].seg = seg;
    g_scene_pending[at].seq = seq;
    g_scene_pending[at].prio = prio;
    g_scene_pending[at].target = target;
    g_scene_pending[at].width = width;
    g_scene_pending[at].height = height;
    g_scene_pending[at].draws = draws;
    nvn_scene_queued++;
    pthread_mutex_unlock(&g_scene_lock);
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] scene queue handle=%#llx seg=%u depth=%u draw=%u\n",
                (unsigned long long)handle, seg, g_scene_pending_count, nvn_nvn_draws);
}

unsigned nvn_scene_take(nvn_scene_item_t *out, unsigned max) {
    pthread_mutex_lock(&g_scene_lock);
    unsigned n = g_scene_pending_count < max ? g_scene_pending_count : max;
    for (unsigned i = 0; i < n; i++) out[i] = g_scene_pending[i];
    g_scene_pending_count = 0;
    pthread_mutex_unlock(&g_scene_lock);
    return n;
}

void nvn_retired_clear_locked(nvn_retired_t *e) {
    if (!e) return;
    for (uint32_t i = 0; i < e->ncmds; i++)
        nvn_cmd_payload_free(&e->cmds[i]);
    for (uint32_t i = 0; i < e->nseg; i++) {
        if (!e->seg) break;
        nvn_prog_release(e->seg[i].pstate);
        for (unsigned v = 0; v < 16; v++) {
            free(e->seg[i].vb_copy[v]);
            for (int st = 0; st < 2; st++) free(e->seg[i].ubo_copy[st][v]);
        }
    }
    free(e->cmds);
    free(e->bind);
    free(e->seg);
    memset(e, 0, sizeof *e);
}

int nvn_ui_seg_want(void) {
    return nvn_trace_ui_segs() && nvn_ui_seg_logs < 512;
}

const char *nvn_seg_reason(const nvn_seg_t *sg, const nvn_retired_t *e) {
    if (!sg->ncmds) return "no-commands";
    if (!sg->have_layout) return "no-layout";
    if (sg->window_texture) return "window-texture";
    if (!sg->color0_tex) return "no-color0";
    if (!sg->draw_count && !sg->clear_count) return "state-only";
    if (sg->draw_count == 0) return "clear-only";
    if (!(sg->have_last && sg->last.have_state) && !(e && e->bind))
        return "no-bind-state";
    if (!sg->warm) return "not-warmed";
    if (!sg->pstate) return "no-pstate";
    if (!sg->pipeline) return "no-pipeline";
    return "ok";
}

int nvn_seg_is_ui(const nvn_seg_t *sg) {
    return sg && ((sg->width == 360 && sg->height == 69) ||
                  (sg->width == 180 && sg->height == 34));
}

/* Drop handles whose command memory is being reused (nvnCommandBufferInitialize
 * on the same guest object): NVN invalidates the old handle at that point. */
void nvn_retired_drop_cb(const cb_t *c) {
    if (!c) return;
    pthread_mutex_lock(&g_retired_lock);
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++)
        if (g_retired[i].used && g_retired[i].source == c) nvn_retired_clear_locked(&g_retired[i]);
    pthread_mutex_unlock(&g_retired_lock);
}

/* Attach the warmed pipeline and its host state to each retired segment.  The
 * scene queue may render a segment frames after the recording thread retired
 * it; the guest can finalize the NVNprogram in between. */
void nvn_seg_prepare(nvn_seg_t *segs, uint32_t nseg, cb_t *c) {
    if (!segs || !nseg || !c || !c->prog) return;
    prog_t *p = c->prog;
    if (p->magic != NVN_MAGIC('P', 'R', 'O', 'G')) return;
    unsigned warmed = 0, tried = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        nvn_seg_t *sg = &segs[i];
        if (!sg->draw_count || !sg->have_layout || sg->window_texture || !sg->color0_tex)
            continue;
        tried++;
        const nvn_bind_state_t *b = (sg->have_last && sg->last.have_state)
                                  ? &sg->last.state : c->bind_state;
        if (!b) continue;
        cb_t view;
        memset(&view, 0, sizeof view);
        view.magic = NVN_MAGIC('C', 'M', 'D', 'B');
        view.dev = c->dev;
        view.cmds = c->cmds + sg->first_cmd;
        view.ncmds = sg->ncmds;
        view.bind_state = (void *)b;
        view.prog = c->prog;
        view.win = c->win;
        view.tex_pool = c->tex_pool;
        view.samp_pool = c->samp_pool;
        view.draw_count = (int)sg->draw_count;
        GfxVertexLayout input;
        uint64_t sig = 0;
        nvn_build_vertex_layout(b, &input, &sig);
        char variant[192];
        nvn_variant_suffix(p, &view, variant, sizeof variant);
        GfxReflectedPipeline *rp =
            nvn_program_cached_pipeline(p, (uint32_t)sg->layout.color_formats[0],
                                        &sg->layout, 0,
                                        input.nattributes ? &input : NULL, sig,
                                        variant, 1);
        if (!rp) continue;
        /* A queued segment replays after the recording thread moved on, so the
         * guest ring buffer may already hold newer data.  This protects the
         * large (>=1024 px) passes; small UI segments only need a bounded
         * probe (nvn_ui_vb_changed), because copying every small segment's
         * vertex/UBO state for a 21-segment recording exhausts memory. */
        if (sg->width >= 1024 && getenv("TOTK_ANDROID_SEG_COPY")) {
            for (int i = 0; i < 16; i++) {
                if (!(b->vb_mask & (1u << i)) || !b->vb[i].address || !b->vb[i].size)
                    continue;
                size_t n = b->vb[i].size > (64u << 20) ? (64u << 20) : b->vb[i].size;
                uint8_t *copy = malloc(n);
                if (!copy) continue;
                memcpy(copy, (const void *)(uintptr_t)b->vb[i].address, n);
                sg->vb_copy[i] = copy;
                sg->vb_copy_size[i] = n;
            }
            for (int st = 0; st < 2; st++)
                for (int slot = 0; slot < 16; slot++) {
                    if (!(b->ubo_mask[st] & (1u << slot)) || !b->ubo[st][slot].address ||
                        !b->ubo[st][slot].size) continue;
                    size_t n = b->ubo[st][slot].size;
                    uint8_t *copy = malloc(n);
                    if (!copy) continue;
                    memcpy(copy, (const void *)(uintptr_t)b->ubo[st][slot].address, n);
                    sg->ubo_copy[st][slot] = copy;
                    sg->ubo_copy_size[st][slot] = n;
                }
        }
        pthread_mutex_lock(&g_prog_lock);
        prog_shader_state_t *s = prog_state_find(p);
        if (s) {
            s->refs++;
            sg->pstate = s;
            sg->pipeline = rp;
            memcpy(sg->variant, variant, sizeof sg->variant);
            sg->warm = 1;
            if ((b->vb_mask & 1u) && b->vb[0].address) {
                memcpy(sg->vb_head, (const void *)(uintptr_t)b->vb[0].address,
                       sizeof sg->vb_head);
                sg->have_vb_head = 1;
            }
            warmed++;
        }
        pthread_mutex_unlock(&g_prog_lock);
    }
    if (getenv("TOTK_ANDROID_TRACE_SCENE_STEPS")) {
        static unsigned shown;
        if (shown++ < 40)
            fprintf(stderr, "[totk-scene-step] prepare nseg=%u tried=%u warmed=%u prog=%p\n",
                    nseg, tried, warmed, (void *)p);
    }
}

/* Before the window composite draws, make its sampled 0x3f source fresh even
 * when the FIFO is deep.  TOTK records ~100 UI targets per frame but the
 * Adreno submit path cannot replay all of them in one call, so the plain FIFO
 * would still be empty at the 0x3f segment when the window samples it.
 *
 * The UI recording is a chain: seg 2 paints the base 360x69 image with 60
 * vertices, segs 3..19 each draw 6 vertices on top of the previous target, and
 * the last segment draws the result into the 1920x1080 0x3f texture.  Ordered
 * by "samples a texture" the base pass and the 0x3f pass sort last, so a
 * one-segment-per-submit FIFO starves exactly the two segments that matter and
 * the compositor samples an untouched target.  Render that recording's small
 * UI segments in recording order here, then the segment that writes the
 * sampled target. */
void nvn_scene_submit_sampled_target(const nvn_bind_state_t *bs, cb_t *view) {
    if (!bs) return;
    if (getenv("TOTK_ANDROID_NO_SAMPLED_DRAIN")) return;
    const void *target = NULL;
    const tex_pool_t *tp = nvn_tex_pool_for(view);
    for (int st = 0; st < 2 && !target; st++)
        for (int slot = 0; slot < 128; slot++) {
            if (!(bs->tex_mask[st][slot / 32] & (1u << (slot % 32)))) continue;
            uint64_t h = bs->tex[st][slot];
            const tex_t *t = tp ? nvn_tex_pool_lookup(tp, (int)(h & 0xfffff)) : NULL;
            if (!t) {
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_tex_get(st, slot, &gh, &gt) && gh == h) t = gt;
            }
            if (t && t->d.format == 0x3f) {
                target = t;
                break;
            }
        }
    if (!target) return;
    uint64_t handle = 0;
    uint32_t seg = 0;
    int found = 0;
    pthread_mutex_lock(&g_scene_lock);
    /* Newest first: an older recording's segments are stale, because the
     * guest re-records the same guest textures every frame. */
    for (unsigned i = g_scene_pending_count; i-- > 0; ) {
        if (g_scene_pending[i].target != target) {
           /* The window sampler and the queued color0 can be different NVN
             * texture objects aliasing one resource; compare resources. */
            const tex_t *pt = nvn_tex_registry_find(g_scene_pending[i].target);
            if (!pt || !nvn_tex_same_resource(pt, target)) continue;
        }
        handle = g_scene_pending[i].handle;
        seg = g_scene_pending[i].seg;
        memmove(&g_scene_pending[i], &g_scene_pending[i + 1],
                (g_scene_pending_count - i - 1) * sizeof g_scene_pending[0]);
        g_scene_pending_count--;
        found = 1;
        break;
    }
    if (found) {
        /* The whole recording is about to run in order below; keep its other
         * entries out of the FIFO so stale copies cannot crowd it. */
        unsigned w = 0;
        for (unsigned i = 0; i < g_scene_pending_count; i++) {
            if (g_scene_pending[i].handle == handle) continue;
            g_scene_pending[w++] = g_scene_pending[i];
        }
        g_scene_pending_count = w;
    }
    pthread_mutex_unlock(&g_scene_lock);
    if (!found) return;
    /* Collect the recording's earlier drawable small-target segments. */
    uint32_t chain[64];
    unsigned nchain = 0;
    unsigned max_chain = 32;
    const char *mc = getenv("TOTK_ANDROID_UI_CHAIN_MAX");
    if (mc && *mc) {
        long v = strtol(mc, NULL, 0);
        if (v >= 0 && v <= 64) max_chain = (unsigned)v;
    }
    pthread_mutex_lock(&g_retired_lock);
    nvn_retired_t *e = NULL;
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++)
        if (g_retired[i].used && g_retired[i].handle == handle) { e = &g_retired[i]; break; }
    if (e) {
        for (uint32_t i = 0; i < e->nseg && i < seg && nchain < max_chain; i++) {
            const nvn_seg_t *sg = &e->seg[i];
            if (!sg->draw_count || !sg->have_layout || sg->window_texture ||
                !sg->color0_tex) continue;
            if (sg->width > 512 || sg->height > 512) continue;
            chain[nchain++] = i;
        }
    }
    pthread_mutex_unlock(&g_retired_lock);
    if (nvn_ui_seg_want())
        fprintf(stderr, "[totk-ui-seg] chain handle=%#llx seg=%u nchain=%u\n",
                (unsigned long long)handle, seg, nchain);
    for (unsigned i = 0; i < nchain; i++)
        nvn_submit_scene_snapshot(handle, chain[i]);
    nvn_submit_scene_snapshot(handle, seg);
}

/* Called at the start of every queue submit: render the retired offscreen
 * passes before the window compositor that samples them. */
void nvn_scene_submit_pending(void) {
    nvn_scene_item_t items[NVN_SCENE_QUEUE_MAX];
    /* Drain a bounded batch per submit instead of one segment.  A UI recording
     * is a chain of ~20 segments (a 60-draw producer, then 6-draw consumers,
     * then the 1920x1080 composite); draining one per submit means the
     * consumers always outrun the producer and the chain never completes.
     * At most one large (>=1024 px) target is replayed per submit: running
     * several full-screen passes in one submit is what the Adreno submit path
     * could not take. */
    unsigned max = 8;
    const char *ns = getenv("TOTK_ANDROID_SCENE_N_PER_SUBMIT");
    if (ns && *ns) {
        long v = strtol(ns, NULL, 0);
        if (v > 0 && v <= (long)NVN_SCENE_QUEUE_MAX) max = (unsigned)v;
    }
    /* A heavy recording can queue hundreds of segments; a fixed batch of 8
     * leaves the backlog growing every frame, so scale the batch with the
     * queue depth (still bounded, and still at most one >=1024 px target). */
    {
        pthread_mutex_lock(&g_scene_lock);
        unsigned depth = g_scene_pending_count;
        pthread_mutex_unlock(&g_scene_lock);
        unsigned want = depth / 4;
        if (want > max) max = want > 64 ? 64 : want;
    }
    unsigned n;
    if (max < NVN_SCENE_QUEUE_MAX) {
        pthread_mutex_lock(&g_scene_lock);
        n = 0;
        unsigned nlarge = 0;
        while (n < g_scene_pending_count && n < max) {
            if (g_scene_pending[n].width >= 1024 || g_scene_pending[n].height >= 1024) {
                if (nlarge) break;
                nlarge = 1;
            }
            n++;
        }
        for (unsigned i = 0; i < n; i++) items[i] = g_scene_pending[i];
        if (n) {
            memmove(&g_scene_pending[0], &g_scene_pending[n],
                    (g_scene_pending_count - n) * sizeof g_scene_pending[0]);
            g_scene_pending_count -= n;
        }
        unsigned depth = g_scene_pending_count;
        pthread_mutex_unlock(&g_scene_lock);
        if (nvn_ui_seg_want() && (n || depth))
            fprintf(stderr, "[totk-ui-seg] drain max=%u n=%u depth_after=%u"
                            " queued_total=%u rendered=%u dropped=%u\n",
                    max, n, depth, nvn_scene_queued, nvn_scene_rendered,
                    nvn_scene_dropped);
    } else {
        n = nvn_scene_take(items, max);
    }
    for (unsigned i = 0; i < n; i++) {
        if (nvn_ui_seg_want())
            fprintf(stderr, "[totk-ui-seg] drain[%u] handle=%#llx seg=%u prio=%u"
                            " target=%p %ux%u draws=%u\n", i,
                    (unsigned long long)items[i].handle, items[i].seg, items[i].prio,
                    items[i].target, items[i].width, items[i].height, items[i].draws);
        nvn_submit_scene_snapshot(items[i].handle, items[i].seg);
    }
}

/* probe-nvn-segment-targets: a synthetic retired recording clears and draws
 * target A, then switches to target B, clears and draws it.  The recording must
 * split into two draw-bearing segments, submit each into its own host target,
 * and the sampled textures must report the generation of the segment that drew
 * to them.  A final target that only clears must be reported as empty. */
int nvn_seg_probe_tex(tex_t *t, uint32_t w, uint32_t h) {
    memset(t, 0, sizeof *t);
    t->magic = NVN_MAGIC('T', 'E', 'X', 0);
    t->d.target = NVN_TARGET_2D;
    t->d.format = 0x25;
    t->d.width = w;
    t->d.height = h;
    t->d.depth = 1;
    t->d.levels = 1;
    t->d.samples = 1;
    t->d.stride = w;
    t->L.size = (uint64_t)w * h * 4;
    t->L.layer_size = t->L.size;
    t->L.layers = 1;
    t->mem = calloc(1, (size_t)w * h * 4);
    if (!t->mem) return 0;
    nvn_tex_registry_add(t);
    return 1;
}
