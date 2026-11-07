/* NVN module: window. */
#include "nvn_internal.h"

static uint8_t nvn_default_native[0x40];

/* Window registry used by command-buffer translation: NVN binds render
 * targets, not windows, so the host resolves the window from its color
 * texture. */
static win_t *g_wins[64];

static unsigned g_nwins;

/* ------------------------------------------------------------------ */
/* Window                                                              */

void nvnWindowBuilderSetDefaults(win_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}

void nvnWindowBuilderSetDevice(win_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnWindowBuilderSetNativeWindow(win_builder_t *b, NvnNativeWindow w) { if (b) b->native = w; }

void nvnWindowBuilderSetTextures(win_builder_t *b, int n, void *const *t) {
    if (!b) return;
    if (n < 0 || n > 4) { nvn_fail("nvnWindowBuilderSetTextures", "texture count out of range"); n = n < 0 ? 0 : 4; }
    b->ntex = n;
    for (int i = 0; i < n; i++) b->tex[i] = t ? t[i] : NULL;
}

void nvnWindowBuilderSetPresentInterval(win_builder_t *b, int v) { if (b) b->interval = v; }

void nvnWindowBuilderSetNumActiveTextures(win_builder_t *b, int n) {
    if (!b) return;
    if (n < 1 || n > 4) { nvn_fail("nvnWindowBuilderSetNumActiveTextures", "out of range"); return; }
    b->active = n;
}

void *nvnWindowBuilderGetNativeWindow(const win_builder_t *b) { return b ? b->native : NULL; }

int nvnWindowBuilderGetPresentInterval(const win_builder_t *b) { return b ? b->interval : 0; }

int nvnWindowBuilderGetNumTextures(const win_builder_t *b) { return b ? b->ntex : 0; }

void *nvnWindowBuilderGetTexture(const win_builder_t *b, int i) {
    return b && i >= 0 && i < b->ntex ? b->tex[i] : NULL;
}

void *nvnWindowBuilderGetDevice(const win_builder_t *b) { return b ? b->dev : NULL; }

int nvnWindowBuilderGetNumActiveTextures(const win_builder_t *b) {
    return b ? (b->active ? b->active : b->ntex) : 0;
}

void nvn_window_register(win_t *w) {
    for (unsigned i = 0; i < g_nwins; i++) if (g_wins[i] == w) return;
    if (g_nwins < sizeof g_wins / sizeof g_wins[0]) g_wins[g_nwins++] = w;
}

void nvn_window_unregister(win_t *w) {
    for (unsigned i = 0; i < g_nwins; i++) {
        if (g_wins[i] != w) continue;
        g_wins[i] = g_wins[--g_nwins];
        return;
    }
}

win_t *nvn_window_for_texture(const void *texture) {
    if (!texture) return NULL;
    for (unsigned i = 0; i < g_nwins; i++) {
        win_t *w = g_wins[i];
        if (!w || w->magic != NVN_MAGIC('W', 'I', 'N', 'D')) continue;
        for (int j = 0; j < w->ntex; j++)
            if (w->tex[j] == texture) return w;
    }
    return NULL;
}

/* Guest window textures (nvnWindowBuilderSetTextures) are the authority for
 * the window's host target size/format.  Only a builder without a usable NVN
 * texture falls back to the VI-shaped 1920x1080 RGBA8 description. */
void nvn_window_guest_desc(const win_builder_t *b, uint32_t *width,
                                  uint32_t *height, uint32_t *format) {
    *width = *height = 0;
    *format = 0;
    if (!b) return;
    for (int i = 0; i < b->ntex; i++) {
        const tex_t *t = nvn_tex_registry_find(b->tex[i]);
        if (!t || !t->d.width || !t->d.height) continue;
        *width = t->d.width;
        *height = t->d.height;
        *format = (uint32_t)t->d.format;
        return;
    }
}

uint32_t nvnWindowInitialize(win_t *w, const win_builder_t *b) { nvn_api("nvnWindowInitialize");
    if (!w || !b) { nvn_fail("nvnWindowInitialize", "null window/builder"); return 0; }
    if (b->ntex < 1 || b->ntex > 4) {
        char d[48]; snprintf(d, sizeof d, "texture count %d", b->ntex);
        nvn_fail("nvnWindowInitialize", d); return 0;
    }
    memset(w, 0, sizeof *w);
    w->magic = NVN_MAGIC('W', 'I', 'N', 'D');
    w->native = b->native ? b->native : nvn_default_native;
    /* The VI/app-shell handoff is not complete yet.  If the guest gave us a
     * native window this host cannot present to, create a real offscreen host
     * window so the cached shader pipeline has a target instead of silently
     * dropping every draw. */
    if (!runtime_native_window_valid(w->native)) {
        uint32_t gw = 0, gh = 0, gf = 0;
        nvn_window_guest_desc(b, &gw, &gh, &gf);
        if (!gw || !gh) { gw = 1920; gh = 1080; }
        if (!gf) gf = 0x25;   /* no guest texture: VI's default RGBA8 */
        void *host = runtime_native_window_create(gw, gh, gf, 0);
        if (host) {
            w->native = host;
            w->owned_native = 1;
            if (nvn_trace())
                fprintf(stderr, "[totk-nvn] WindowInitialize host target %ux%u format=%#x"
                                " (guest texture driven)\n", gw, gh, gf);
        }
    }
    w->ntex = b->ntex;
    w->interval = b->interval ? b->interval : 1;
    w->active = b->active ? b->active : b->ntex;
    if (w->active > w->ntex) w->active = w->ntex;
    memcpy(w->tex, b->tex, sizeof w->tex);
    nvn_window_register(w);
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] WindowInitialize %p native=%p textures=%d interval=%d active=%d\n",
                (void *)w, w->native, w->ntex, w->interval, w->active);
    return 1;
}

void nvnWindowFinalize(win_t *w) {
    if (!w) return;
    nvn_window_unregister(w);
    if (w->owned_native && w->native) runtime_native_window_destroy(w->native);
    w->native = NULL;
    w->owned_native = 0;
    w->magic = 0;
}

void nvnWindowSetDebugLabel(win_t *w, const char *l) { (void)w; (void)l; }

int nvnWindowAcquireTexture(win_t *w, sync_t *s, int *index) { nvn_api("nvnWindowAcquireTexture");
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnWindowAcquireTexture")) return 1;
    if (!index) { nvn_fail("nvnWindowAcquireTexture", "null index"); return 1; }
    *index = w->ntex ? (w->next++ % w->ntex) : 0;
    if (s && nvn_obj(s, NVN_MAGIC('S', 'Y', 'N', 'C'), "nvnWindowAcquireTexture(sync)")) {
        /* The returned texture is usable only once earlier GPU work finished. */
        if (!nvn_gpu_drain("nvnWindowAcquireTexture")) return 1;
        if (!s->fence) s->fence = gfx_backend_fence_create();
        if (s->fence) {
            gfx_backend_fence_signal((GfxFence *)s->fence);
            s->state = 1;
            s->fence_generation++;
        } else {
            nvn_fail("nvnWindowAcquireTexture", "cannot create acquire fence");
        }
    }
    return 0;
}

void *nvnWindowGetNativeWindow(const win_t *w) { nvn_api("nvnWindowGetNativeWindow");
    if (!w || w->magic != NVN_MAGIC('W', 'I', 'N', 'D')) return nvn_default_native;
    return w->native ? w->native : nvn_default_native;
}

int nvnWindowGetPresentInterval(const win_t *w) { return w ? w->interval : 0; }

void nvnWindowSetPresentInterval(win_t *w, int v) { if (w) w->interval = v; }

void nvnWindowSetCrop(win_t *w, int x, int y, int ww, int hh) {
    if (!w) return;
    w->crop[0] = x; w->crop[1] = y; w->crop[2] = ww; w->crop[3] = hh;
}

void nvnWindowGetCrop(const win_t *w, int *out) {
    if (!out) return;
    if (!w) { memset(out, 0, 16); return; }
    memcpy(out, w->crop, 16);
}

void nvnWindowSetNumActiveTextures(win_t *w, int n) {
    if (!w) return;
    if (n < 1 || n > w->ntex) { nvn_fail("nvnWindowSetNumActiveTextures", "out of range"); return; }
    w->active = n;
}

int nvnWindowGetNumActiveTextures(const win_t *w) { return w ? w->active : 0; }

int nvnWindowGetNumTextures(const win_t *w) { return w ? w->ntex : 0; }

/* The window's host target layout comes from the guest window texture when the
 * guest supplied one; only a bare window falls back to the VI description.
 * The guest renders its final composite into those textures, so their format
 * is the one the compositor must match (VI's descriptor is RGBA8 even when the
 * engine uses a 16-bit HDR target). */
void nvn_window_layout(const win_t *w, GfxRenderTargetLayout *layout,
                              uint32_t *width, uint32_t *height) {
    memset(layout, 0, sizeof *layout);
    *width = *height = 0;
    uint32_t ww = 0, wh = 0, wf = 0;
    if (w) {
        for (int i = 0; i < w->ntex; i++) {
            const tex_t *t = nvn_tex_registry_find(w->tex[i]);
            if (!t || !t->d.width || !t->d.height) continue;
            ww = t->d.width;
            wh = t->d.height;
            wf = (uint32_t)t->d.format;
            break;
        }
    }
    if (!ww || !wh) {
        if (w && w->native) runtime_native_window_info(w->native, &ww, &wh, &wf, NULL, NULL);
    }
    *width = ww;
    *height = wh;
    uint32_t fmt = gfx_backend_vk_format_for_nvn(wf ? wf : 0x25);
    if (!fmt) fmt = gfx_backend_vk_format_for_nvn(0x25);
    layout->color_count = 1;
    layout->color_formats[0] = fmt;
    layout->depth_format = 0;
}

uint32_t nvn_window_format(const win_t *w) {
    uint32_t format = 0;
    if (w && runtime_native_window_valid(w->native)) {
        if (runtime_native_window_info(w->native, NULL, NULL, &format, NULL, NULL) && format)
            return format;
    }
    return 0x25;
}

/* Copy color attachment 0 of a host MRT image into the window's CPU mirror so
 * the existing NativeWindow readback/present path keeps observing the frame. */
int nvn_window_mirror_target(win_t *w, GfxOffscreenTarget *target) {
    uint32_t ww = 0, wh = 0;
    void *buf = NULL;
    size_t bsz = 0;
    if (!w || !target || !runtime_native_window_info(w->native, &ww, &wh, NULL, &buf, NULL) ||
        !buf || !runtime_native_window_info(w->native, NULL, NULL, NULL, NULL, &bsz))
        return 0;
    uint32_t tw = gfx_backend_offscreen_width(target);
    uint32_t th = gfx_backend_offscreen_height(target);
    if (!tw || !th) return 0;
    if (tw != ww || th != wh) {
        static unsigned logged;
        if (logged++ < 4)
            fprintf(stderr, "[totk-nvn] WARNING MRT target %ux%u cannot mirror %ux%u window\n",
                    tw, th, ww, wh);
        return 0;
    }
    size_t need = (size_t)ww * wh * 4;
    if (need > bsz) return 0;
    size_t src_size = 0;
    const uint8_t *src = gfx_backend_offscreen_pixels(target, &src_size);
    if (src && src_size >= need) {
        memcpy(buf, src, need);
        return 1;
    }
    uint8_t *dst = buf;
    for (uint32_t y = 0; y < wh; y++)
        for (uint32_t x = 0; x < ww; x++)
            if (!gfx_backend_offscreen_read_pixel_index(target, 0, x, y, dst + ((size_t)y * ww + x) * 4))
                return 0;
    return 1;
}

/* The 0x3f texture the window compositor samples, resolved through the same
 * pool/global tables the translator uses. */
const tex_t *nvn_window_sampled_tex(const nvn_bind_state_t *bs, cb_t *view) {
    if (!bs || !view) return NULL;
    const tex_pool_t *tp = nvn_tex_pool_for(view);
    for (int st = 0; st < 2; st++)
        for (int slot = 0; slot < 128; slot++) {
            if (!(bs->tex_mask[st][slot / 32] & (1u << (slot % 32)))) continue;
            uint64_t h = bs->tex[st][slot];
            const tex_t *t = tp ? nvn_tex_pool_lookup(tp, (int)(h & 0xfffff)) : NULL;
            if (!t) {
                uint64_t gh = 0;
                const tex_t *gt = NULL;
                if (nvn_global_tex_get(st, slot, &gh, &gt) && gh == h) t = gt;
            }
            if (t && t->d.format == 0x3f) return t;
        }
    return NULL;
}

/* Pick the RT that actually backs the window's sampled 0x3f texture.  The
 * bridge is authoritative: the queued text segment sets it through
 * nvn_tex_set_rt.  Fall back to the last large scene target only if the
 * sampled texture has never been associated. */
GfxOffscreenTarget *nvn_window_present_rt(const nvn_bind_state_t *bs, cb_t *view,
                                                 const tex_t **tex_out) {
    const tex_t *t = nvn_window_sampled_tex(bs, view);
    if (tex_out) *tex_out = t;
    if (t) {
        tex_bridge_t *b = nvn_tex_bridge(t, 0);
        if (nvn_trace_composite()) {
            static unsigned src_guest, src_rt;
            int valid = b && b->rt_valid;
            unsigned *n = valid ? &src_rt : &src_guest;
            if (*n < 4) {
                (*n)++;
                size_t ms = t->L.size ? (size_t)t->L.size
                                      : (size_t)t->d.width * t->d.height;
                uint32_t nz = t->mem ? nvn_first_nonzero(t->mem, ms, 1u << 20) : UINT32_MAX;
                fprintf(stderr, "[totk-frame] window present source tex=%p %ux%u fmt=%#x"
                                " rt_valid=%d rt=%p gen=%llu/%llu gfx=%p guest_first_nz=%s%u"
                                " -> src=%s\n",
                        (const void *)t, t->d.width, t->d.height, t->d.format,
                        valid, b ? (void *)b->rt : NULL,
                        b ? (unsigned long long)b->rt_frame : 0ull,
                        b ? (unsigned long long)b->rt_uploaded : 0ull,
                        b ? b->gfx : NULL,
                        nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz,
                        valid ? "rt-gen" : (t->mem ? "guest" : "none"));
            }
        }
        if (b && b->rt_valid && b->rt) return b->rt;
    }
    return g_last_scene_rt;
}

/* Headless present is an offscreen target in the host window (there is no real
 * surface to hand the present-target image to).  Frame dumps must follow the
 * same target present() would use on a surface, or they show the overlay
 * target instead of the scene the window pass actually samples. */
int nvn_window_frame_ppm(win_t *w, const char *path) {
    if (!w || !path) return 0;
    GfxOffscreenTarget *pt = runtime_native_window_present_target(w->native);
    if (pt) return gfx_backend_offscreen_write_ppm_index(pt, 0, path);
    return runtime_native_window_screenshot(w->native, path);
}
