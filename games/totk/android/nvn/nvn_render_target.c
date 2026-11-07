/* NVN module: render_target. */
#include "nvn_internal.h"

/* Title/save UI creates a distinct host target per UI plane/atlas; 16 was
 * exhausted before the save-slot screen could render.  The host images are
 * shared per (size, layout, guest owner), so this only bounds the number of
 * live guest targets, not frames. */
static nvn_host_rt_t g_host_rts[128];

static unsigned g_host_rt_count;

static uint64_t g_host_rt_clock;

static unsigned g_host_rt_creates;

static unsigned g_host_rt_evictions;

static pthread_mutex_t g_host_rt_lock = PTHREAD_MUTEX_INITIALIZER;

/* Targets that received clears/state but no draw in a retired recording.  The
 * window compositor may sample them; report that explicitly instead of
 * presenting the guest bytes as rendered content. */
static struct { const void *tex; } g_empty_targets[NVN_EMPTY_TARGET_MAX];

static unsigned g_empty_next;

static pthread_mutex_t g_empty_lock = PTHREAD_MUTEX_INITIALIZER;

uint64_t nvn_layout_sig(const GfxRenderTargetLayout *l) {
    if (!l) return 0;
    uint64_t h = 1469598103934665603ull;
    uint64_t v = ((uint64_t)l->color_count << 32) | (uint32_t)l->depth_format;
    h ^= v; h *= 1099511628211ull;
    for (uint32_t i = 0; i < l->color_count && i < GFX_MAX_COLOR_TARGETS; i++) {
        h ^= (uint64_t)(uint32_t)l->color_formats[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

int nvn_layout_equal(const GfxRenderTargetLayout *a, const GfxRenderTargetLayout *b) {
    if (!a || !b || a->color_count != b->color_count || a->depth_format != b->depth_format)
        return 0;
    for (uint32_t i = 0; i < a->color_count && i < GFX_MAX_COLOR_TARGETS; i++)
        if (a->color_formats[i] != b->color_formats[i]) return 0;
    return 1;
}

int nvn_layout_valid(const GfxRenderTargetLayout *l) {
    if (!l || l->color_count < 1 || l->color_count > GFX_MAX_COLOR_TARGETS) return 0;
    for (uint32_t i = 0; i < l->color_count; i++)
        if (!l->color_formats[i]) return 0;
    return 1;
}

GfxOffscreenTarget *nvn_host_rt_get_owner(uint32_t width, uint32_t height,
                                                 const GfxRenderTargetLayout *layout,
                                                 const void *owner) {
    if (!width || !height || !nvn_layout_valid(layout)) return NULL;
    pthread_mutex_lock(&g_host_rt_lock);
    for (unsigned i = 0; i < g_host_rt_count; i++) {
        if (g_host_rts[i].width != width || g_host_rts[i].height != height) continue;
        if (g_host_rts[i].owner != owner) continue;
        if (!nvn_layout_equal(&g_host_rts[i].layout, layout)) continue;
        GfxOffscreenTarget *gfx = g_host_rts[i].gfx;
        g_host_rts[i].last_use = ++g_host_rt_clock;
        pthread_mutex_unlock(&g_host_rt_lock);
        return gfx;
    }
    /* The boot logo records a fresh guest texture (and therefore a fresh
     * owner) for each animation frame.  Without eviction the 128-entry cache
     * fills and every later SetRenderTargets fails forever.  Create the new
     * image first, then replace the least-recently-used entry that is not the
     * current present target. */
    GfxOffscreenTarget *gfx = gfx_backend_offscreen_create_layout(width, height, layout);
    if (!gfx) {
        pthread_mutex_unlock(&g_host_rt_lock);
        return NULL;
    }
    unsigned slot = g_host_rt_count;
    if (slot >= sizeof g_host_rts / sizeof g_host_rts[0]) {
        uint64_t oldest = UINT64_MAX;
        unsigned victim = UINT_MAX;
        for (unsigned i = 0; i < g_host_rt_count; i++) {
            if (!g_host_rts[i].gfx) continue;
            if (g_host_rts[i].gfx == g_last_scene_rt ||
                g_host_rts[i].gfx == g_present_rt) continue;
            if (g_host_rts[i].last_use < oldest) {
                oldest = g_host_rts[i].last_use;
                victim = i;
            }
        }
        if (victim == UINT_MAX) {
            pthread_mutex_unlock(&g_host_rt_lock);
            gfx_backend_offscreen_destroy(gfx);
            nvn_fail("nvnCommandBufferSetRenderTargets", "host render-target cache full (all pinned)");
            return NULL;
        }
        slot = victim;
        uint32_t vw = g_host_rts[victim].width, vh = g_host_rts[victim].height;
        const void *vo = g_host_rts[victim].owner;
        GfxOffscreenTarget *old = g_host_rts[victim].gfx;
        g_host_rt_evictions++;
        if (g_host_rt_evictions <= 8 || (g_host_rt_evictions % 64) == 0)
            fprintf(stderr, "[totk-nvn] host rt evict #%u %ux%u owner=%p age=%llu"
                            " (creates=%u live=%u)\n", g_host_rt_evictions,
                    vw, vh, vo, (unsigned long long)oldest,
                    g_host_rt_creates, g_host_rt_count);
        /* Drop sampled-texture associations before destroying the image. */
        nvn_tex_forget_rt(old);
        gfx_backend_offscreen_destroy(old);
    } else {
        g_host_rt_count++;
    }
    g_host_rts[slot].width = width;
    g_host_rts[slot].height = height;
    g_host_rts[slot].layout = *layout;
    g_host_rts[slot].owner = owner;
    g_host_rts[slot].gfx = gfx;
    g_host_rts[slot].last_use = ++g_host_rt_clock;
    g_host_rt_creates++;
    if (g_host_rt_creates <= 32 || nvn_trace() || (g_host_rt_evictions &&
                                                   (g_host_rt_creates % 64) == 0))
        fprintf(stderr, "[totk-nvn] host render target %ux%u colors=%u depth=%#x"
                        " owner=%p created (creates=%u live=%u evictions=%u)\n",
                width, height, layout->color_count, layout->depth_format, owner,
                g_host_rt_creates, g_host_rt_count, g_host_rt_evictions);
    pthread_mutex_unlock(&g_host_rt_lock);
    return gfx;
}

void nvn_host_rt_clear(void) {
    pthread_mutex_lock(&g_host_rt_lock);
    for (unsigned i = 0; i < g_host_rt_count; i++) {
        if (g_host_rts[i].gfx) {
            nvn_tex_forget_rt(g_host_rts[i].gfx);
            gfx_backend_offscreen_destroy(g_host_rts[i].gfx);
        }
        g_host_rts[i].gfx = NULL;
    }
    g_host_rt_count = 0;
    g_host_rt_clock = 0;
    g_host_rt_creates = g_host_rt_evictions = 0;
    g_present_rt = NULL;
    g_last_scene_rt = NULL;
    pthread_mutex_unlock(&g_host_rt_lock);
}

int nvn_rt_layout_from_targets(uint32_t n, void *const *tex,
                                      const void *const *views,
                                      const void *depth, const void *depth_view,
                                      GfxRenderTargetLayout *layout,
                                      uint32_t *width, uint32_t *height,
                                      const char **why) {
    memset(layout, 0, sizeof *layout);
    *width = *height = 0;
    if (why) *why = NULL;
    if (n < 1 || n > GFX_MAX_COLOR_TARGETS) {
        if (why) *why = "color target count";
        return 0;
    }
    /* NVN MRT arrays are sparse: a null slot is inactive, but the active
     * slots after it keep their NVN colour index.  The backend layout cannot
     * express VK_ATTACHMENT_UNUSED, so fill inactive slots with a dummy
     * attachment using the first active format; shader SV_Target indices stay
     * intact and unused slots are never sampled. */
    uint32_t active_n = n;
    while (active_n > 1 && tex && !tex[active_n - 1]) active_n--;
    int base_fmt = 0;
    for (uint32_t i = 0; i < active_n; i++) {
        const tex_t *t = nvn_tex_registry_find(tex ? tex[i] : NULL);
        if (!t) {
            if (!tex || !tex[i]) {
                if (!base_fmt) {
                    /* Find the first active slot before accepting a gap. */
                    for (uint32_t j = i + 1; j < active_n; j++) {
                        const tex_t *tj = nvn_tex_registry_find(tex[j]);
                        if (tj) {
                            int nf = tj->d.format;
                            if (views && views[j]) nvn_view_format(views[j], &nf, NULL, NULL);
                            int f = gfx_backend_vk_format_for_nvn((uint32_t)nf);
                            if (f) { base_fmt = f; *width = tj->d.width; *height = tj->d.height; break; }
                        }
                    }
                }
                if (!base_fmt) {
                    nvn_trace_bad_target("color", i, NULL);
                    if (why) *why = "inactive color target has no active format";
                    return 0;
                }
                layout->color_formats[i] = (uint32_t)base_fmt;
                continue;
            }
            nvn_trace_bad_target("color", i, tex[i]);
            if (why) *why = "color target is not an NVN texture";
            return 0;
        }
        /* A view can change the attachment format (and its subresource
         * range); the host attachment must use the view's format. */
        int nf = t->d.format;
        int view_fmt = 0;
        uint32_t vbase_layer = 0, vlayers = 0;
        if (views && views[i]) {
            view_fmt = nvn_view_format(views[i], &nf, &vbase_layer, &vlayers);
            if (view_fmt && vlayers > 1) {
                static unsigned vl;
                if (vl++ < 8)
                    fprintf(stderr, "[totk-nvn] SetRenderTargets color%u is a view"
                                    " with %u layers (host renders layer %u only)\n",
                            i, vlayers, vbase_layer);
            }
        }
        int fmt = gfx_backend_vk_format_for_nvn((uint32_t)nf);
        if (!fmt) {
            if (why) *why = "unsupported color target format";
            return 0;
        }
        if (!base_fmt) base_fmt = fmt;
        layout->color_formats[i] = fmt;
        if (view_fmt && nf != t->d.format) {
            static unsigned vf;
            /* Always report a BC4/R8-style view mismatch; rate-limit the rest. */
            if (vf++ < 64 || t->d.format == 0x49)
                fprintf(stderr, "[totk-nvn] rt-view color%u base fmt=%#x -> view fmt=%#x"
                                " vk=%#x %ux%u\n", i, t->d.format, nf, fmt,
                        t->d.width, t->d.height);
        }
        if (!*width || !*height) { *width = t->d.width; *height = t->d.height; }
        else if (t->d.width != *width || t->d.height != *height) {
            static unsigned logged;
            if (logged++ < 8)
                fprintf(stderr, "[totk-nvn] WARNING color target %u is %ux%u, color0 is %ux%u\n",
                        i, t->d.width, t->d.height, *width, *height);
        }
    }
    layout->color_count = active_n;
    if (depth) {
        const tex_t *dt = nvn_tex_registry_find(depth);
        if (!dt) {
            nvn_trace_bad_target("depth", 0, depth);
            if (why) *why = "depth target is not an NVN texture";
            return 0;
        }
        int nf = dt->d.format;
        if (depth_view) nvn_view_format(depth_view, &nf, NULL, NULL);
        int fmt = gfx_backend_vk_format_for_nvn((uint32_t)nf);
        if (!fmt) { if (why) *why = "unsupported depth target format"; return 0; }
        layout->depth_format = fmt;
        if (!*width) { *width = dt->d.width; *height = dt->d.height; }
    }
    if (!*width || !*height) { if (why) *why = "render target has no dimensions"; return 0; }
    return 1;
}

void nvn_note_empty_target(const void *tex) {
    if (!tex) return;
    pthread_mutex_lock(&g_empty_lock);
    g_empty_targets[g_empty_next++ % NVN_EMPTY_TARGET_MAX].tex = tex;
    pthread_mutex_unlock(&g_empty_lock);
    if (nvn_trace_frame() || nvn_trace_inputs())
        fprintf(stderr, "[totk-frame] target-empty tex=%p (clears/state only, no draws)\n",
                tex);
}

int nvn_is_empty_target(const void *tex) {
    int found = 0;
    if (!tex) return 0;
    pthread_mutex_lock(&g_empty_lock);
    for (unsigned i = 0; i < NVN_EMPTY_TARGET_MAX; i++)
        if (g_empty_targets[i].tex == tex) { found = 1; break; }
    pthread_mutex_unlock(&g_empty_lock);
    return found;
}

/* True when the command buffer's target set is exactly the window's single
 * color target: draws and present then share the swapchain image instead of a
 * separate MRT image. */
int nvn_layout_matches_window(const win_t *w, const GfxRenderTargetLayout *layout,
                                     uint32_t width, uint32_t height) {
    if (!layout || layout->color_count != 1 || layout->depth_format) return 0;
    GfxRenderTargetLayout wl;
    uint32_t ww = 0, wh = 0;
    nvn_window_layout(w, &wl, &ww, &wh);
    if (ww != width || wh != height) return 0;
    if (wl.color_formats[0] == layout->color_formats[0]) return 1;
    /* A final composite rendered into the window's own texture with a
     * different format still belongs to the window pass: the host presents
     * through the window's format (the gfx present path converts).  Same size
     * and the texture being a window texture is enough. */
    static unsigned logged;
    if (logged++ < 4)
        fprintf(stderr, "[totk-nvn] window pass layout %#x vs window %#x %ux%u:"
                        " presenting through the window format\n",
                layout->color_formats[0], wl.color_formats[0], ww, wh);
    return 1;
}
