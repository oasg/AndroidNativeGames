/* NVN module: texture_1. */
#include "nvn_internal.h"

int runtime_nvn_texture_upload_guest(void *texture) {
    tex_t *t = texture;
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return 0;
    if (!t->mem) {
        fprintf(stderr, "[totk-nvn] ERROR guest texture upload without storage (target=%d)\n",
                t->d.target);
        return 0;
    }
    int gtarget = nvn_texture_gfx_target(t->d.target);
    if (gtarget < 0) {
        unsigned logged;
        if (logged++ < 8)
            fprintf(stderr, "[totk-nvn] ERROR guest texture target %d has no host equivalent\n",
                    t->d.target);
        return 0;
    }
    if (t->d.samples > 1) {
        static unsigned logged;
        if (logged++ < 8)
            fprintf(stderr, "[totk-nvn] ERROR guest texture target=%d samples=%d: MSAA upload unsupported\n",
                    t->d.target, t->d.samples);
        return 0;
    }
    int vkfmt = gfx_backend_vk_format_for_nvn((uint32_t)t->d.format);
    if (!vkfmt) {
        static unsigned logged;
        if (logged++ < 8)
            fprintf(stderr, "[totk-nvn] ERROR guest texture format %#x has no Vulkan mapping (target=%d)\n",
                    t->d.format, t->d.target);
        return 0;
    }
    uint32_t levels = t->d.levels ? t->d.levels : 1;
    uint32_t layers = nvn_texture_gfx_layers(&t->d);
    uint32_t w = t->d.width, h = t->d.height;
    uint32_t depth = t->d.depth ? t->d.depth : 1;
    if (!w || !h || !layers || levels > 16) {
        fprintf(stderr, "[totk-nvn] ERROR guest texture shape %ux%ux%u levels=%u layers=%u\n",
                w, h, depth, levels, layers);
        return 0;
    }
    int is_3d = gtarget == GFX_TEXTURE_3D;

    /* Count subresources first: one per (level, layer) for 2D/cube/array,
     * one per (level, depth slice) for 3D. */
    uint32_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t rb = 0, rows = 0, slices = 0, bpb = 0;
        if (runtime_guest_texture_level_geom(t->d.target, t->d.format, t->d.flags,
                                             t->d.samples, w, h, depth, levels,
                                             t->d.stride, t->d.pkg_block_h,
                                             l, &rb, &rows, &slices, &bpb) ||
            !rb || !rows || !slices || !bpb) {
            fprintf(stderr, "[totk-nvn] ERROR guest texture level %u geometry unsupported"
                            " (target=%d format=%#x %ux%ux%u)\n",
                    l, t->d.target, t->d.format, w, h, depth);
            return 0;
        }
        total += is_3d ? 1 : layers;
    }
    if (!total) return 0;

    GfxTextureSubresource *subs = calloc(total, sizeof *subs);
    uint8_t **blocks = calloc(total, sizeof *blocks);
    if (!subs || !blocks) { free(subs); free(blocks); return 0; }

    uint32_t nsub = 0;
    int ok = 1;
    for (uint32_t l = 0; l < levels && ok; l++) {
        uint32_t rb = 0, rows = 0, slices = 0, bpb = 0;
        if (runtime_guest_texture_level_geom(t->d.target, t->d.format, t->d.flags,
                                             t->d.samples, w, h, depth, levels,
                                             t->d.stride, t->d.pkg_block_h,
                                             l, &rb, &rows, &slices, &bpb) ||
            !rb || !rows || !slices || !bpb) {
            ok = 0;
            break;
        }
        size_t slice_bytes = (size_t)rb * rows;
        if (is_3d) {
            /* The backend takes one subresource per mip level for a 3D image,
             * with all depth slices contiguous.  Passing one subresource per
             * slice made it reject the image (size < depth*slice). */
            uint8_t *buf = malloc(slice_bytes * (slices ? slices : 1));
            if (!buf) { ok = 0; break; }
            for (uint32_t z = 0; z < slices && ok; z++) {
                if (runtime_guest_texture_deswizzle_at(t->mem, buf + (size_t)z * slice_bytes,
                                                       rb, t->d.target, t->d.format,
                                                       t->d.flags, t->d.samples, w, h, depth,
                                                       levels, t->d.stride, t->d.pkg_block_h,
                                                       l, 0, z, 1)) {
                    fprintf(stderr, "[totk-nvn] ERROR deswizzle level=%u z=%u"
                                    " (target=%d format=%#x)\n",
                            l, z, t->d.target, t->d.format);
                    free(buf);
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
            blocks[nsub] = buf;
            subs[nsub].level = l;
            subs[nsub].layer = 0;
            subs[nsub].data = buf;
            subs[nsub].size = slice_bytes * slices;
            nsub++;
        } else {
            for (uint32_t layer = 0; layer < layers && ok; layer++) {
                uint8_t *buf = malloc(slice_bytes ? slice_bytes : 1);
                if (!buf) { ok = 0; break; }
                if (runtime_guest_texture_deswizzle_at(t->mem, buf, rb, t->d.target,
                                                       t->d.format, t->d.flags, t->d.samples,
                                                       w, h, depth, levels, t->d.stride,
                                                       t->d.pkg_block_h, l, layer, 0, 1)) {
                    fprintf(stderr, "[totk-nvn] ERROR deswizzle level=%u layer=%u"
                                    " (target=%d format=%#x)\n",
                            l, layer, t->d.target, t->d.format);
                    free(buf);
                    ok = 0;
                    break;
                }
                if (l == 0 && layer == 0 && t->d.width == 96 && t->d.height == 96 &&
                    t->d.format == 0x49) {
                    const char *dp = getenv("TOTK_ANDROID_TEX_DUMP");
                    static int dumped;
                    if (dp && *dp && !dumped) {
                        dumped = 1;
                        char path[PATH_MAX];
                        snprintf(path, sizeof path, "%s-96x96.bin", dp);
                        FILE *df = fopen(path, "wb");
                        if (df) {
                            fwrite(buf, 1, slice_bytes, df);
                            fclose(df);
                            fprintf(stderr, "[totk-nvn] dumped font atlas %s (%zu bytes)\n",
                                    path, slice_bytes);
                        }
                    }
                }
                blocks[nsub] = buf;
                subs[nsub].level = l;
                subs[nsub].layer = layer;
                subs[nsub].data = buf;
                subs[nsub].size = slice_bytes;
                nsub++;
            }
        }
    }

    GfxTexture *g = NULL;
    if (ok && nsub == total) {
        GfxTextureDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.target = (uint32_t)gtarget;
        desc.vk_format = (uint32_t)vkfmt;
        desc.width = w;
        desc.height = h;
        desc.depth = is_3d ? depth : 1;
        desc.mip_levels = levels;
        desc.array_layers = is_3d ? 1 : layers;
        desc.samples = 1;
        g = gfx_backend_texture_create_desc(&desc, subs, nsub);
        if (!g)
            fprintf(stderr, "[totk-nvn] ERROR texture create target=%d format=%#x %ux%ux%u"
                            " levels=%u layers=%u\n",
                    t->d.target, t->d.format, w, h, depth, levels, layers);
    }
    for (uint32_t i = 0; i < nsub; i++) free(blocks[i]);
    free(blocks);
    free(subs);
    if (!g) return 0;
    int stored = nvn_texture_store_bridge(t, g);
    if (!stored) fprintf(stderr, "[totk-nvn] ERROR texture bridge full\n");
    return stored;
}

void *nvn_texture_gfx(const tex_t *t) {
    pthread_mutex_lock(&g_tex_bridge_lock);
    tex_bridge_t *b = nvn_tex_bridge(t, 0);
    void *g = b ? b->gfx : NULL;
    pthread_mutex_unlock(&g_tex_bridge_lock);
    return g;
}

void nvn_upload_trace_result(int on, const tex_t *t, const char *what,
                                    GfxTexture *g) {
    if (!on) return;
    fprintf(stderr, "[totk-frame] upload result=%s tex=%p gfx=%p bridge=%u/%u\n",
            what, (const void *)t, (void *)g, g_tex_bridge_count, g_tex_bridge_cap);
}

/* The font atlas is written by the guest through plain memory, not through
 * nvnTextureWriteTexels/FlushTexels.  Sample its storage and keep a checksum
 * in the bridge so a change can invalidate the cached host image. */
int nvn_tex_is_direct_mask(const tex_t *t) {
    return t && (t->d.format == 0x1 || t->d.format == 0x3d) &&
           ((t->d.width == 2048 && t->d.height == 4096) ||
            (t->d.width == 780 && t->d.height == 60));
}

uint64_t nvn_tex_mem_sum(const tex_t *t) {
    if (!t || !t->mem) return 0;
    size_t n = t->L.size ? (size_t)t->L.size
                         : (size_t)t->d.width * t->d.height;
    size_t stride = n > (1u << 20) ? 256 : 16;
    uint64_t sum = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i += stride)
        sum = (sum ^ t->mem[i]) * 1099511628211ULL;
    return sum;
}

/* First bind uploads the guest block-linear texture.  The macOS runtime does
 * the equivalent on texture creation; the Android path keeps the guest storage
 * as the source of truth and uploads lazily.  A texture that is also a scene
 * render target is different: the freshly rendered target image wins over the
 * stale guest bytes, so rebuild the sampled texture from the RT readback. */
GfxTexture *nvn_texture_gfx_ready_locked(const tex_t *t) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return NULL;
    tex_bridge_t *b = nvn_tex_bridge(t, 0);
    nvn_mask_trace(t, "bind");
    /* Direct guest writes to the atlas/glyph cache are invisible to the NVN
     * texel API; checksum the storage and rebuild the host copy on change.
     * The atlas is 8 MiB and binds happen every frame, so sample it on a
     * fraction of the binds: a delayed rebuild still lands in the same frame. */
    if (b && nvn_tex_is_direct_mask(t)) {
        static unsigned sum_tick;
        int due = !b->mem_sum_valid || ((sum_tick++ & 7) == 0);
        if (due) {
            uint64_t s = nvn_tex_mem_sum(t);
            if (b->mem_sum_valid && s != b->mem_sum) {
                b->gfx_dirty = 1;
                static unsigned reloads;
                if (reloads++ < 16)
                    fprintf(stderr, "[totk-nvn] mask reload tex=%p %ux%u sum=%016llx\n",
                            (const void *)t, t->d.width, t->d.height,
                            (unsigned long long)s);
            }
            b->mem_sum = s;
            b->mem_sum_size = t->L.size;
        }
        b->mem_sum_valid = 1;
    }
    /* The guest wrote texels since the host copy was uploaded: drop the stale
     * CPU-uploaded image so the normal path below re-uploads the new bytes.
     * This does not touch the render-target association; if the texture is an
     * RT the RT readback path below rebuilds it anyway. */
    if (b && b->gfx_dirty) {
        b->gfx_dirty = 0;
        if (b->gfx) {
            gfx_backend_texture_destroy((GfxTexture *)b->gfx);
            b->gfx = NULL;
            b->rt_uploaded = 0;
            static unsigned dirty_logged;
            if (dirty_logged++ < 8)
                fprintf(stderr, "[totk-nvn] tex-rewrite tex=%p %#x %ux%u: cached host"
                                " copy dropped (guest wrote texels)\n", (const void *)t,
                        t->d.format, t->d.width, t->d.height);
        }
    }
    int utr = nvn_trace_inputs() && nvn_upload_traces < 512 &&
              t->d.width <= 1024 && t->d.height <= 1024;
    if (utr) {
        nvn_upload_traces++;
        size_t ms = t->L.size ? (size_t)t->L.size
                              : (size_t)t->d.width * t->d.height * 4;
        uint32_t nz = nvn_first_nonzero(t->mem, ms, 1u << 20);
        fprintf(stderr, "[totk-frame] upload begin tex=%p fmt=%#x tgt=%d %ux%u lv=%u"
                        " layers=%u mem=%p off=%lld msize=%zu first_nz=%s%u"
                        " bridge=%u/%u gfx=%p rt=%d gen=%llu/%llu\n",
                (const void *)t, t->d.format, t->d.target, t->d.width, t->d.height,
                t->d.levels, t->L.layers, (void *)t->mem, (long long)t->off, ms,
                nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz,
                g_tex_bridge_count, g_tex_bridge_cap, (void *)nvn_texture_gfx(t),
                b ? b->rt_valid : 0,
                b ? (unsigned long long)b->rt_frame : 0,
                b ? (unsigned long long)b->rt_uploaded : 0);
    }
    /* A target that only received clears/state in the latest recording has no
     * segment to render.  Say so instead of presenting stale guest bytes as
     * if a scene pass had drawn to it. */
    if ((!b || !b->rt_valid) && nvn_is_empty_target(t)) {
        static const void *logged;
        if (logged != (const void *)t) {
            logged = t;
            nvn_empty_sampled++;
            if (nvn_trace_frame())
                fprintf(stderr, "[totk-frame] tex-empty tex=%p %ux%u fmt=%#x"
                                " (no segment drew to this target)\n",
                        (const void *)t, t->d.width, t->d.height, t->d.format);
        }
    }
    if (b && b->rt_valid && b->rt_frame != b->rt_uploaded &&
        t->d.target == NVN_TARGET_2D && t->d.levels <= 1 &&
        t->d.width == b->rt_width && t->d.height == b->rt_height) {
        int vkfmt = gfx_backend_vk_format_for_nvn((uint32_t)t->d.format);
        /* When the render target was attached through a view, its format can
         * differ from the base texture (a BC4 font atlas rendered through an
         * R8 view).  The render target is the authority for the pixels, so
         * sample it with the render target's format; requiring the base
         * format here would drop the whole atlas. */
        int rtf = b->rt_layout.color_count >= 1 ? b->rt_layout.color_formats[0] : 0;
        int sample_fmt = (rtf && rtf != vkfmt) ? rtf : vkfmt;
        if (sample_fmt != vkfmt) {
            static unsigned view_logged;
            if (view_logged++ < 8)
                fprintf(stderr, "[totk-nvn] tex-from-rt view format tex=%p base fmt=%#x"
                                " vk=%#x rt vk=%#x %ux%u\n", (const void *)t,
                        t->d.format, vkfmt, rtf, b->rt_width, b->rt_height);
        }
        int bpp = sample_fmt ? gfx_backend_vk_format_bpp(sample_fmt) : 0;
        size_t need = (size_t)b->rt_width * b->rt_height * (size_t)bpp;
        if (sample_fmt && bpp > 0 && b->rt_layout.color_count >= 1 && need) {
            size_t have = 0;
            const uint8_t *px = gfx_backend_offscreen_pixels(b->rt, &have);
            if (px && have >= need) {
                GfxTextureDesc d;
                memset(&d, 0, sizeof d);
                d.target = GFX_TEXTURE_2D;
                d.vk_format = (uint32_t)sample_fmt;
                d.width = b->rt_width;
                d.height = b->rt_height;
                d.depth = 1;
                d.mip_levels = 1;
                d.array_layers = 1;
                d.samples = 1;
                GfxTextureSubresource sub = { 0, 0, px, need };
                GfxTexture *g = gfx_backend_texture_create_desc(&d, &sub, 1);
                if (g) {
                    if (b->gfx) gfx_backend_texture_destroy((GfxTexture *)b->gfx);
                    b->gfx = g;
                    b->rt_uploaded = b->rt_frame;
                    if (nvn_trace_frame()) {
                        const uint32_t *w0 = (const uint32_t *)px;
                        size_t nz = need / 4;
                        for (size_t q = 0; q < need / 4; q++) if (w0[q]) { nz = q; break; }
                        fprintf(stderr, "[totk-frame] tex-from-rt tex=%p gfx=%p rt=%p gen=%llu"
                                        " vkfmt=%#x %ux%u bytes=%zu first=%08x %08x %08x %08x"
                                        " first_nz=%s%#zx\n", (const void *)t, (void *)g,
                                (void *)b->rt, (unsigned long long)b->rt_frame, sample_fmt,
                                b->rt_width, b->rt_height, need,
                                w0[0], w0[1], w0[2], w0[3],
                                nz == need / 4 ? "none " : "", nz == need / 4 ? 0 : nz * 4);
                    }
                    nvn_upload_trace_result(utr, t, "rt", g);
                    return g;
                }
            }
        }
        nvn_upload_trace_result(utr, t, "rt-fallback", NULL);
        if (nvn_trace_frame())
            fprintf(stderr, "[totk-frame] tex-from-rt fallback tex=%p fmt=%#x vkfmt=%#x"
                            " rtfmt=%#x %ux%u vs %ux%u levels=%u\n", (const void *)t,
                    t->d.format, vkfmt,
                    b->rt_layout.color_count ? b->rt_layout.color_formats[0] : 0,
                    b->rt_width, b->rt_height, t->d.width, t->d.height, t->d.levels);
    }
    GfxTexture *g = nvn_texture_gfx(t);
    if (g) { nvn_upload_trace_result(utr, t, "cached", g); return g; }
    int up = runtime_nvn_texture_upload_guest((void *)t);
    g = nvn_texture_gfx(t);
    nvn_upload_trace_result(utr, t, up ? "guest" : "fail", g);
    return up ? g : NULL;
}

GfxTexture *nvn_texture_gfx_ready(const tex_t *t) {
    pthread_mutex_lock(&g_tex_bridge_lock);
    GfxTexture *g = nvn_texture_gfx_ready_locked(t);
    pthread_mutex_unlock(&g_tex_bridge_lock);
    return g;
}

void nvnTextureFinalize(tex_t *t) {
    if (!t) return;
    nvn_tex_registry_del(t);
    pthread_mutex_lock(&g_tex_bridge_lock);
    for (unsigned i = 0; i < g_tex_bridge_count; i++) {
        if (g_tex_bridges[i].tex == t) {
            if (g_tex_bridges[i].gfx) gfx_backend_texture_destroy(g_tex_bridges[i].gfx);
            g_tex_bridges[i] = g_tex_bridges[--g_tex_bridge_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_tex_bridge_lock);
    t->magic = 0;
}

void nvnTextureSetDebugLabel(tex_t *t, const char *l) { (void)t; (void)l; }

int nvnTextureGetStorageClass(const tex_t *t) { return t && t->d.target == NVN_TARGET_BUFFER ? 0 : 1; }

int nvnTextureGetFlags(const tex_t *t) { return t ? t->d.flags : 0; }

int nvnTextureGetTarget(const tex_t *t) { return t ? t->d.target : 0; }

int nvnTextureGetWidth(const tex_t *t) { return t ? (int)t->d.width : 0; }

int nvnTextureGetHeight(const tex_t *t) { return t ? (int)t->d.height : 0; }

int nvnTextureGetDepth(const tex_t *t) { return t ? (int)t->d.depth : 0; }

int nvnTextureGetLevels(const tex_t *t) { return t ? (int)t->d.levels : 0; }

int nvnTextureGetFormat(const tex_t *t) { return t ? t->d.format : 0; }

int nvnTextureGetSamples(const tex_t *t) { return t ? t->d.samples : 0; }

int nvnTextureGetStride(const tex_t *t) { return t ? (int)t->d.stride : 0; }

void nvnTextureGetSwizzle(const tex_t *t, int *r, int *g, int *bl, int *a) {
    (void)t; if (r) *r = 2; if (g) *g = 3; if (bl) *bl = 4; if (a) *a = 5;
}

int nvnTextureGetDepthStencilMode(const tex_t *t) { (void)t; return 0; }

int nvnTextureGetStorageSize(const tex_t *t) { return t ? (int)t->L.size : 0; }

int64_t nvnTextureGetViewOffset(const tex_t *t, const void *view) { nvn_api("nvnTextureGetViewOffset");
    if (!t || !view) return 0;
    const uint8_t *v = view;
    int set_levels = v[0] & 1, set_layers = (v[0] >> 1) & 1;
    unsigned base_level = v[1], min_layer = (unsigned)v[4] | ((unsigned)v[5] << 8);
    uint64_t off = set_levels ? nvn_host_tex_level_offset(&t->d, base_level) : 0;
    if (set_layers) off += (uint64_t)min_layer * t->L.layer_size;
    return (int64_t)off;
}

uint64_t nvnTextureGetTextureAddress(const tex_t *t) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return 0;
    return t->mem ? (uint64_t)(uintptr_t)t->mem : 0;
}

void nvnTextureGetSparseTileLayout(const tex_t *t, void *out) { (void)t; if (out) memset(out, 0, 0x28); }

void nvnTextureFlushTexels(const tex_t *t, const void *v, const void *r) { (void)v; (void)r; nvn_texture_mark_dirty(t); }

void nvnTextureInvalidateTexels(const tex_t *t, const void *v, const void *r) { (void)v; (void)r; nvn_texture_mark_dirty(t); }

void *nvnTextureGetMemoryPool(const tex_t *t) { return t ? t->pool : NULL; }

int64_t nvnTextureGetMemoryOffset(const tex_t *t) { return t ? t->off : 0; }

int nvnTextureGetRawStorageClass(const tex_t *t) { (void)t; return 1; }

size_t nvnTextureGetZCullStorageSize(const tex_t *t) { (void)t; return 0; }

uint64_t nvnTextureGetDebugID(const tex_t *t) { return t ? (uint64_t)(uintptr_t)t : 0; }

uint32_t nvnTextureCompare(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    return !memcmp(&a->d, &b->d, sizeof a->d) && a->mem == b->mem;
}

/* Diagnostic: dump the first vertices of stream 0 as 16-bit halves plus the
 * resolved vertex layout, so an attribute offset/format mismatch is visible. */
float nvn_half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e ? ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15)
                : (float)m / 1024.0f;
    return s ? -v : v;
}
