/* NVN module: texture_2. */
#include "nvn_internal.h"

void nvn_texture_mark_dirty(const tex_t *t) {
    if (!t) return;
    g_mask_writes++;
    nvn_mask_trace(t, "cpu-write");
    pthread_mutex_lock(&g_tex_bridge_lock);
    tex_bridge_t *b = nvn_tex_bridge(t, 1);
    if (b) b->gfx_dirty = 1;
    pthread_mutex_unlock(&g_tex_bridge_lock);
}

/* A CPU-side texel copy is enough for this host: nothing consumes the GPU
 * layout, and ReadTexels round-trips the same bytes. */
void nvn_tex_copy(const tex_t *t, const void *view, const int *region,
                         void *dst, const void *src, ptrdiff_t row, ptrdiff_t img, int to_dst) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !t->mem || !region) return;
    if (to_dst ? !dst : !src) return;
    int bw = 1, bh = 1; nvn_fmt_t f = nvn_fmt_of(t->d.format);
    bw = f.bw ? f.bw : 1; bh = f.bh ? f.bh : 1;
    int x = region[0], y = region[1], z = region[2];
    int w = region[3], h = region[4], d = region[5];
    (void)x; (void)y; (void)z;
    if (w <= 0 || h <= 0 || d <= 0) return;
    size_t rb = (size_t)((w + bw - 1) / bw) * (f.bpb ? f.bpb : 1);
    if (row <= 0) row = (ptrdiff_t)rb;
    if (img <= 0) img = row * ((h + bh - 1) / bh);
    uint64_t off = (uint64_t)nvnTextureGetViewOffset(t, view);
    if (off >= t->L.size) return;
    size_t avail = (size_t)(t->L.size - off);
    if (!to_dst) {
        if ((size_t)(img * d) > avail) d = (int)(avail / (size_t)(img ? img : 1));
        for (int zz = 0; zz < d; zz++)
            for (int yy = 0; yy < h; yy++)
                memcpy(t->mem + off + (size_t)zz * img + (size_t)yy * row,
                       (const uint8_t *)src + (size_t)zz * img + (size_t)yy * row,
                       rb < (size_t)row ? rb : (size_t)row);
        /* The guest just changed the texels.  Mark the host copy stale; it is
         * rebuilt from these bytes on the next bind (destroying it here would
         * race with a concurrent bind, so the flag is the hand-off). */
        nvn_texture_mark_dirty(t);
        if (nvn_ui_seg_want()) {
            size_t ms = t->L.size ? (size_t)t->L.size
                                  : (size_t)t->d.width * t->d.height;
            uint32_t nz = nvn_first_nonzero(t->mem, ms, 1u << 20);
            static unsigned wl;
            if (wl++ < 24)
                fprintf(stderr, "[totk-ui-seg] tex-write tex=%p %#x %ux%u tgt=%d off=%llu"
                                " bytes=%zu first_nz=%s%u\n", (const void *)t,
                        t->d.format, t->d.width, t->d.height, t->d.target,
                        (unsigned long long)off, (size_t)rb * (size_t)h,
                        nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz);
        }
    } else {
        if ((size_t)(img * d) > avail) d = (int)(avail / (size_t)(img ? img : 1));
        for (int zz = 0; zz < d; zz++)
            for (int yy = 0; yy < h; yy++)
                memcpy((uint8_t *)dst + (size_t)zz * img + (size_t)yy * row,
                       t->mem + off + (size_t)zz * img + (size_t)yy * row,
                       rb < (size_t)row ? rb : (size_t)row);
    }
}

void nvnTextureWriteTexels(const tex_t *t, const void *v, const int *r, const void *src) { nvn_api("nvnTextureWriteTexels");
    nvn_tex_writes++;
    nvn_tex_copy(t, v, r, NULL, src, 0, 0, 0);
    nvn_texture_bridge_drop(t);
}

void nvnTextureWriteTexelsStrided(const tex_t *t, const void *v, const int *r,
                                         const void *src, ptrdiff_t row, ptrdiff_t img) { nvn_api("nvnTextureWriteTexelsStrided");
    nvn_tex_writes++;
    nvn_tex_copy(t, v, r, NULL, src, row, img, 0);
    nvn_texture_bridge_drop(t);
}

void nvnTextureReadTexels(const tex_t *t, const void *v, const int *r, void *dst) { nvn_api("nvnTextureReadTexels");
    nvn_tex_copy(t, v, r, dst, NULL, 0, 0, 1);
}

void nvnTextureReadTexelsStrided(const tex_t *t, const void *v, const int *r,
                                        void *dst, ptrdiff_t row, ptrdiff_t img) { nvn_api("nvnTextureReadTexelsStrided");
    nvn_tex_copy(t, v, r, dst, NULL, row, img, 1);
}

/* Texture view: 16 bytes, the layout TOTK uses. */
void nvnTextureViewSetDefaults(uint8_t *v) { if (v) memset(v, 0, NVN_TEXTURE_VIEW_SIZE); }

/* NVNtextureView (16 bytes): flags byte 0, base_level byte 1, nlevels byte 2,
 * target byte 3, min_layer u16 at 4, nlayers u16 at 6, format u16 at 8,
 * swizzle bytes 10..13.  This matches the guest's 16-byte view layout. */
void nvnTextureViewSetLevels(uint8_t *v, int base, int n) {
    if (!v || base < 0 || base > 255 || n < 0 || n > 255) { nvn_fail("nvnTextureViewSetLevels", "out of range"); return; }
    v[0] |= 1; v[1] = (uint8_t)base; v[2] = (uint8_t)n;
}

void nvnTextureViewSetLayers(uint8_t *v, int min, int n) {
    if (!v || min < 0 || min > 65535 || n < 0 || n > 65535) { nvn_fail("nvnTextureViewSetLayers", "out of range"); return; }
    v[0] |= 2; v[4] = (uint8_t)min; v[5] = (uint8_t)(min >> 8);
    v[6] = (uint8_t)n; v[7] = (uint8_t)(n >> 8);
}

void nvnTextureViewSetFormat(uint8_t *v, int f) {
    if (!v) return; v[0] |= 4; v[8] = (uint8_t)f; v[9] = (uint8_t)(f >> 8);
}

void nvnTextureViewSetSwizzle(uint8_t *v, int r, int g, int bl, int a) {
    if (!v) return; v[0] |= 16; v[10] = (uint8_t)r; v[11] = (uint8_t)g; v[12] = (uint8_t)bl; v[13] = (uint8_t)a;
}

void nvnTextureViewSetDepthStencilMode(uint8_t *v, int m) { (void)v; (void)m; }

void nvnTextureViewSetTarget(uint8_t *v, int t) { if (v) { v[0] |= 8; v[3] = (uint8_t)t; } }

uint32_t nvnTextureViewGetLevels(const uint8_t *v, int *base, int *n) {
    if (!v || !base || !n) return 0;
    *base = v[1]; *n = v[2]; return v[0] & 1;
}

uint32_t nvnTextureViewGetLayers(const uint8_t *v, int *min, int *n) {
    if (!v || !min || !n) return 0;
    *min = v[4] | (v[5] << 8); *n = v[6] | (v[7] << 8); return (v[0] >> 1) & 1;
}

uint32_t nvnTextureViewGetFormat(const uint8_t *v, int *f) {
    if (!v || !f) return 0; *f = v[8] | (v[9] << 8); return (v[0] >> 2) & 1;
}

uint32_t nvnTextureViewGetSwizzle(const uint8_t *v, int *r, int *g, int *bl, int *a) {
    if (!v || !r || !g || !bl || !a) return 0;
    *r = v[10]; *g = v[11]; *bl = v[12]; *a = v[13]; return (v[0] >> 4) & 1;
}

uint32_t nvnTextureViewGetDepthStencilMode(const uint8_t *v, int *m) {
    if (!v || !m) return 0; *m = 0; return 0;
}

uint32_t nvnTextureViewGetTarget(const uint8_t *v, int *t) {
    if (!v || !t) return 0; *t = v[3]; return (v[0] >> 3) & 1;
}

uint32_t nvnTextureViewCompare(const uint8_t *a, const uint8_t *b) {
    return a && b && !memcmp(a, b, NVN_TEXTURE_VIEW_SIZE);
}

/* Pools of descriptors.  The map is a chained hash keyed by (pool,id), with
 * a host generation and last-use frame in each pool.  A pool object reused
 * without Initialize has a different n/off/pool signature, so old entries
 * cannot resolve to the new contents.  At cap only stale generations and
 * pools unused for more than two frames are pruned; a live current entry is
 * never evicted. */
int tex_pool_valid(const tex_pool_t *p) {
    return p && p->magic == NVN_MAGIC('T', 'P', 'O', 'L') &&
           p->n > 0 && p->n <= 0x100000 && p->off >= 0 && !(p->off & 3);
}

uint32_t tex_pool_sig(const tex_pool_t *p) {
    return (uint32_t)((uintptr_t)p->pool >> 4) * 2654435761u ^
           (uint32_t)p->off ^ p->n * 40503u;
}

__attribute__((unused)) static void tex_pool_unlink_locked(int32_t idx) {
    unsigned b = pool_home(g_tex_pool_map[idx].pool, (int)g_tex_pool_map[idx].id);
    int32_t prev = -1, cur = g_tex_buckets[b];
    while (cur >= 0) {
        if (cur == idx) {
            if (prev < 0) g_tex_buckets[b] = g_tex_pool_map[cur].next;
            else g_tex_pool_map[prev].next = g_tex_pool_map[cur].next;
            g_tex_pool_map[cur].next = g_tex_free;
            g_tex_free = cur;
            if (g_tex_pool_map_count) g_tex_pool_map_count--;
            return;
        }
        prev = cur; cur = g_tex_pool_map[cur].next;
    }
}

void tex_pool_prune_locked(void) {
    for (unsigned b = 0; b < NVN_POOL_BUCKETS; b++) {
        int32_t prev = -1, cur = g_tex_buckets[b];
        while (cur >= 0) {
            tex_pool_map_t *e = &g_tex_pool_map[cur];
            const tex_pool_t *p = e->pool;
            int dead = !tex_pool_valid(p) || e->gen != p->gen ||
                       e->sig != tex_pool_sig(p) || pool_old(p);
            int32_t next = e->next;
            if (dead) {
                if (prev < 0) g_tex_buckets[b] = next;
                else g_tex_pool_map[prev].next = next;
                e->next = g_tex_free; g_tex_free = cur;
                if (g_tex_pool_map_count) g_tex_pool_map_count--;
            } else {
                prev = cur;
            }
            cur = next;
        }
    }
}

unsigned runtime_nvn_tex_pool_regs(void) { return g_tex_pool_map_count; }

unsigned runtime_nvn_tex_pool_full(void) { return g_tex_pool_full; }

unsigned runtime_nvn_tex_pool_max(void) { return g_tex_pool_max; }

uint32_t nvnTexturePoolInitialize(tex_pool_t *p, const pool_t *pool, ptrdiff_t off, int n) { nvn_api("nvnTexturePoolInitialize");
    if (!p || !nvn_obj(pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnTexturePoolInitialize(pool)")) return 0;
    if (n < 0) { nvn_fail("nvnTexturePoolInitialize", "negative count"); return 0; }
    uint32_t gen = p->magic == NVN_MAGIC('T', 'P', 'O', 'L') ? p->gen + 1 : 1;
    if (!gen) gen = 1;
    p->pool = (pool_t *)pool; p->off = off; p->n = n;
    p->magic = NVN_MAGIC('T', 'P', 'O', 'L');
    p->gen = gen; pool_epoch_set(p);
    return 1;
}

void nvnTexturePoolFinalize(tex_pool_t *p) {
    /* The NVNtexturePool object is guest-owned and may be embedded in an
     * engine object that is still being torn down.  macOS leaves it untouched;
     * writing gen/magic here corrupted the engine's texture-pool list. */
    if (!p) return;
    nvn_tex_pool_forget(p);
    nvn_pool_forget(p);
}

void nvnTexturePoolSetDebugLabel(tex_pool_t *p, const char *l) { (void)p; (void)l; }

void nvnTexturePoolRegisterTexture(const tex_pool_t *p, int id, const tex_t *t, const void *v) { nvn_api("nvnTexturePoolRegisterTexture");
    (void)v;
    static int trace = -1;
    if (trace < 0) trace = getenv("TOTK_ANDROID_TRACE_TEXREG") != NULL;
    if (!p || p->magic != NVN_MAGIC('T', 'P', 'O', 'L') ||
        !nvn_obj(t, NVN_MAGIC('T', 'E', 'X', 0), "nvnTexturePoolRegisterTexture(texture)")) {
        if (trace) fprintf(stderr, "[totk-texreg] reject p=%p id=%d t=%p\n",
                           (const void *)p, id, (const void *)t);
        return;
    }
    if (id < 0 || (uint32_t)id >= p->n) {
        if (trace) fprintf(stderr, "[totk-texreg] reject id p=%p id=%d n=%u\n",
                           (const void *)p, id, p->n);
        nvn_fail("nvnTexturePoolRegisterTexture", "id out of range");
        return;
    }
    if (trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "[totk-texreg] ok p=%p id=%d t=%p fmt=%#x %ux%u lv=%u"
                            " mem=%p off=%lld\n", (const void *)p, id, (const void *)t,
                    (unsigned)t->d.format, t->d.width, t->d.height, t->d.levels,
                    (void *)t->mem, (long long)t->off);
    }
    nvn_tex_pool_add(p, id, t);
}

void nvnTexturePoolRegisterImage(const tex_pool_t *p, int id, const tex_t *t, const void *v) {
    nvnTexturePoolRegisterTexture(p, id, t, v);
}

void *nvnTexturePoolGetMemoryPool(const tex_pool_t *p) { return p ? p->pool : NULL; }

int64_t nvnTexturePoolGetMemoryOffset(const tex_pool_t *p) { return p ? p->off : 0; }

int nvnTexturePoolGetSize(const tex_pool_t *p) { return p ? (int)p->n : 0; }

const char *nvn_tex_target_name(int target) {
    switch (target) {
    case 0: return "1d";
    case 2: return "3d";
    case 3: return "1da";
    case 4: return "2da";
    case 5: return "2dms";
    case 6: return "2dmsa";
    case 8: return "cube";
    case 9: return "cubearr";
    case 10: return "buf";
    default: return "2d";
    }
}

/* One-shot evidence for a segment that rendered a zero target: what the
 * shader was bound to, where each sampled texture comes from, and whether its
 * guest storage was ever populated. */
int nvn_tex_is_ui(const tex_t *t) {
    return t && ((t->d.width == 360 && t->d.height == 69) ||
                 (t->d.width == 180 && t->d.height == 34));
}

size_t nvn_texel_bytes(const tex_t *t) {
    int vkfmt = t ? gfx_backend_vk_format_for_nvn((uint32_t)t->d.format) : 0;
    int bpp = vkfmt ? gfx_backend_vk_format_bpp(vkfmt) : 0;
    return bpp > 0 ? (size_t)bpp : 0;
}

/* A destination that is also a host render target cannot be written through a
 * detached sampled GfxTexture: the compositor samples the render target, not
 * that copy.  Report it instead of silently writing to a throw-away image. */
int nvn_tex_is_rt(const tex_t *t, GfxOffscreenTarget **rt,
                         GfxRenderTargetLayout *layout, uint32_t *rw, uint32_t *rh) {
    tex_bridge_t *b = nvn_tex_bridge(t, 0);
    if (!b || !b->rt_valid || !b->rt) return 0;
    if (rt) *rt = b->rt;
    if (layout) *layout = b->rt_layout;
    if (rw) *rw = b->rt_width;
    if (rh) *rh = b->rt_height;
    return 1;
}

int runtime_nvn_textured_draw_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0x99);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) return 0;

    uint8_t *pool_mem = calloc(1, 0x10000);
    if (!pool_mem) return 0;
    pool_t pool;
    pool_builder_t pb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
    if (!nvnMemoryPoolInitialize(&pool, &pb)) { free(pool_mem); return 0; }
    tex_builder_t tb;
    tex_t tex;
    memset(&tex, 0, sizeof tex);
    nvnTextureBuilderSetDefaults(&tb);
    tb.dev = &d;
    nvnTextureBuilderSetSize2D(&tb, 2, 2);
    nvnTextureBuilderSetFormat(&tb, 0x25);
    nvnTextureBuilderSetLevels(&tb, 1);
    nvnTextureBuilderSetStorage(&tb, &pool, 0);
    if (!nvnTextureInitialize(&tex, &tb)) { free(pool_mem); return 0; }
    const uint8_t checker[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    if (runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                       tex.d.flags, tex.d.samples, tex.d.width, tex.d.height,
                                       tex.d.depth, tex.d.levels, tex.d.stride,
                                       tex.d.pkg_block_h)) { free(pool_mem); return 0; }
    if (!runtime_nvn_texture_upload_guest(&tex)) { free(pool_mem); return 0; }
    void *tex_gfx = nvn_texture_gfx(&tex);
    GfxTexturedPipeline *tp = gfx_backend_builtin_textured_pipeline();
    if (!tex_gfx || !tp) { free(pool_mem); return 0; }

    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) { free(pool_mem); return 0; }
    prog_host_set(&p, NULL, tp, tex_gfx, NULL, 1);
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) { free(pool_mem); return 0; }
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) { free(pool_mem); return 0; }
    for (int i = 0; i < frames; i++) {
        cb.prog = &p;
        cb.win = &w;
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    uint8_t center[4] = {0}, corner[4] = {0};
    runtime_native_window_read_pixel(nw, 640, 360, center);
    runtime_native_window_read_pixel(nw, 0, 0, corner);
    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    int ok = center[0] > 80 && center[0] < 180 && center[1] > 80 && center[1] < 180 &&
             center[2] > 80 && center[2] < 180 && corner[0] < 40;
    free(pool_mem);
    return ok;
}
