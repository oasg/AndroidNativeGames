/* NVN module: texture_0. */
#include "nvn_internal.h"

void nvn_tex_registry_add(tex_t *t) {
    pthread_mutex_lock(&g_tex_obj_lock);
    for (unsigned i = 0; i < g_tex_obj_count; i++) if (g_tex_objs[i] == t) { pthread_mutex_unlock(&g_tex_obj_lock); return; }
    if (g_tex_obj_count == g_tex_obj_cap) {
        unsigned cap = g_tex_obj_cap ? g_tex_obj_cap * 2 : 128;
        tex_t **n = realloc(g_tex_objs, (size_t)cap * sizeof *n);
        if (!n) { pthread_mutex_unlock(&g_tex_obj_lock); return; }
        g_tex_objs = n;
        g_tex_obj_cap = cap;
    }
    g_tex_objs[g_tex_obj_count++] = t;
    pthread_mutex_unlock(&g_tex_obj_lock);
}

void nvn_tex_registry_del(tex_t *t) {
    pthread_mutex_lock(&g_tex_obj_lock);
    for (unsigned i = 0; i < g_tex_obj_count; i++) {
        if (g_tex_objs[i] != t) continue;
        g_tex_objs[i] = g_tex_objs[--g_tex_obj_count];
        break;
    }
    pthread_mutex_unlock(&g_tex_obj_lock);
}

const tex_t *nvn_tex_registry_find(const void *p) {
    const tex_t *found = NULL;
    if (!p) return NULL;
    pthread_mutex_lock(&g_tex_obj_lock);
    for (unsigned i = 0; i < g_tex_obj_count; i++)
        if (g_tex_objs[i] == p) { found = g_tex_objs[i]; break; }
    pthread_mutex_unlock(&g_tex_obj_lock);
    return found;
}

void nvn_global_tex_set(int stage, int slot, uint64_t handle, const tex_t *tex) {
    if (stage < 0 || stage >= 6 || slot < 0 || slot >= 128) return;
    pthread_mutex_lock(&g_global_tex_lock);
    g_global_tex[stage][slot].handle = handle;
    g_global_tex[stage][slot].tex = tex;
    pthread_mutex_unlock(&g_global_tex_lock);
}

int nvn_global_tex_get(int stage, int slot, uint64_t *handle, const tex_t **tex) {
    int ok = 0;
    if (stage < 0 || stage >= 6 || slot < 0 || slot >= 128) return 0;
    pthread_mutex_lock(&g_global_tex_lock);
    if (g_global_tex[stage][slot].tex) {
        if (handle) *handle = g_global_tex[stage][slot].handle;
        if (tex) *tex = g_global_tex[stage][slot].tex;
        ok = 1;
    }
    pthread_mutex_unlock(&g_global_tex_lock);
    return ok;
}

void nvn_global_img_set(int stage, int slot, uint64_t handle, const tex_t *tex) {
    if (stage < 0 || stage >= 6 || slot < 0 || slot >= 16) return;
    pthread_mutex_lock(&g_global_img_lock);
    g_global_img[stage][slot].handle = handle;
    g_global_img[stage][slot].tex = tex;
    pthread_mutex_unlock(&g_global_img_lock);
}

int nvn_global_img_get(int stage, int slot, uint64_t *handle, const tex_t **tex) {
    int ok = 0;
    if (stage < 0 || stage >= 6 || slot < 0 || slot >= 16) return 0;
    pthread_mutex_lock(&g_global_img_lock);
    if (g_global_img[stage][slot].tex) {
        if (handle) *handle = g_global_img[stage][slot].handle;
        if (tex) *tex = g_global_img[stage][slot].tex;
        ok = 1;
    }
    pthread_mutex_unlock(&g_global_img_lock);
    return ok;
}

/* Two NVN texture objects can alias one pool range (a render target and the
 * sampled view of it are often different tex_t objects).  Share the bridge
 * entry so the RT association and sampled GfxTexture are visible through
 * every alias. */
int nvn_tex_same_resource(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    if (a == b) return 1;
    if (a->d.target != b->d.target || a->d.format != b->d.format ||
        a->d.width != b->d.width || a->d.height != b->d.height ||
        a->d.levels != b->d.levels)
        return 0;
    if (a->mem && a->mem == b->mem && a->L.size && a->L.size == b->L.size) return 1;
    if (a->pool && a->pool == b->pool && a->off == b->off &&
        a->L.size && a->L.size == b->L.size)
        return 1;
    return 0;
}

tex_bridge_t *nvn_tex_bridge(const tex_t *t, int create) {
    if (!t) return NULL;
    pthread_mutex_lock(&g_tex_bridge_lock);
    for (unsigned i = 0; i < g_tex_bridge_count; i++)
        if (g_tex_bridges[i].tex == t) {
            tex_bridge_t *b = &g_tex_bridges[i];
            pthread_mutex_unlock(&g_tex_bridge_lock);
            return b;
        }
    for (unsigned i = 0; i < g_tex_bridge_count; i++)
        if (nvn_tex_same_resource(g_tex_bridges[i].tex, t)) {
            tex_bridge_t *b = &g_tex_bridges[i];
            pthread_mutex_unlock(&g_tex_bridge_lock);
            return b;
        }
    if (!create) { pthread_mutex_unlock(&g_tex_bridge_lock); return NULL; }
    if (g_tex_bridge_count == g_tex_bridge_cap) {
        unsigned cap = g_tex_bridge_cap ? g_tex_bridge_cap * 2 : 64;
        tex_bridge_t *n = realloc(g_tex_bridges, (size_t)cap * sizeof *n);
        if (!n) { pthread_mutex_unlock(&g_tex_bridge_lock); return NULL; }
        g_tex_bridges = n;
        g_tex_bridge_cap = cap;
    }
    memset(&g_tex_bridges[g_tex_bridge_count], 0, sizeof g_tex_bridges[0]);
    g_tex_bridges[g_tex_bridge_count].tex = t;
    tex_bridge_t *b = &g_tex_bridges[g_tex_bridge_count++];
    pthread_mutex_unlock(&g_tex_bridge_lock);
    return b;
}

/* Associate a guest colour texture with the host image the scene pass renders
 * into.  The sampled copy is dropped so the next bind rebuilds it from the
 * rendered image instead of the stale guest bytes. */
void nvn_tex_set_rt(const tex_t *t, GfxOffscreenTarget *rt,
                           const GfxRenderTargetLayout *layout, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_tex_bridge_lock);
    tex_bridge_t *b = nvn_tex_bridge(t, 1);
    if (!b || !rt) { pthread_mutex_unlock(&g_tex_bridge_lock); return; }
    int layout_changed = !b->rt_valid || b->rt != rt;
    b->rt = rt;
    b->rt_layout = *layout;
    b->rt_width = w;
    b->rt_height = h;
    b->rt_valid = 1;
    b->rt_frame++;
    b->rt_uploaded = 0;
    if (layout_changed || b->gfx) {
        if (b->gfx) gfx_backend_texture_destroy((GfxTexture *)b->gfx);
        b->gfx = NULL;
    }
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] tex-rt tex=%p rt=%p %ux%u fmt=%#x gen=%llu\n",
                (const void *)t, (void *)rt, w, h, layout->color_formats[0],
                (unsigned long long)b->rt_frame);
    pthread_mutex_unlock(&g_tex_bridge_lock);
}

/* Drop every sampled-texture association that points at an RT about to be
 * destroyed.  The host render-target cache evicts old logo/UI targets; a stale
 * bridge->rt would otherwise dangle and a later sample would read freed memory. */
void nvn_tex_forget_rt(GfxOffscreenTarget *rt) {
    if (!rt) return;
    pthread_mutex_lock(&g_tex_bridge_lock);
    for (unsigned i = 0; i < g_tex_bridge_count; i++) {
        tex_bridge_t *b = &g_tex_bridges[i];
        if (b->rt != rt) continue;
        b->rt = NULL;
        b->rt_valid = 0;
        b->rt_uploaded = 0;
        b->rt_frame = 0;
        if (b->gfx) {
            gfx_backend_texture_destroy((GfxTexture *)b->gfx);
            b->gfx = NULL;
        }
    }
    pthread_mutex_unlock(&g_tex_bridge_lock);
}

/* ------------------------------------------------------------------ */
/* Texture                                                             */

void nvnTextureBuilderSetDefaults(tex_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.target = NVN_TARGET_2D;
    b->d.format = 0x25; /* NVN_FORMAT_RGBA8 */
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}

void nvnTextureBuilderSetDevice(tex_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnTextureBuilderSetFlags(tex_builder_t *b, int f) { if (b) b->d.flags = f; }

void nvnTextureBuilderSetTarget(tex_builder_t *b, int t) { if (b) b->d.target = t; }

void nvnTextureBuilderSetWidth(tex_builder_t *b, int v) { if (b) b->d.width = (uint32_t)v; }

void nvnTextureBuilderSetHeight(tex_builder_t *b, int v) { if (b) b->d.height = (uint32_t)v; }

void nvnTextureBuilderSetDepth(tex_builder_t *b, int v) { if (b) b->d.depth = (uint32_t)v; }

void nvnTextureBuilderSetSize1D(tex_builder_t *b, int w) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = b->d.depth = 1; }
}

void nvnTextureBuilderSetSize2D(tex_builder_t *b, int w, int h) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = (uint32_t)h; b->d.depth = 1; }
}

void nvnTextureBuilderSetSize3D(tex_builder_t *b, int w, int h, int d) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = (uint32_t)h; b->d.depth = (uint32_t)d; }
}

void nvnTextureBuilderSetLevels(tex_builder_t *b, int v) { if (b) b->d.levels = (uint32_t)v; }

void nvnTextureBuilderSetFormat(tex_builder_t *b, int f) { if (b) b->d.format = f; }

void nvnTextureBuilderSetSamples(tex_builder_t *b, int s) { if (b) b->d.samples = s; }

void nvnTextureBuilderSetSwizzle(tex_builder_t *b, int r, int g, int bl, int a) {
    (void)b; (void)r; (void)g; (void)bl; (void)a;
}

void nvnTextureBuilderSetDepthStencilMode(tex_builder_t *b, int m) { (void)b; (void)m; }

void nvnTextureBuilderSetStorage(tex_builder_t *b, void *p, ptrdiff_t off) {
    if (b) { b->pool = p; b->off = off; }
}

void nvnTextureBuilderSetPackagedTextureData(tex_builder_t *b, const void *data) { if (b) b->pkg_data = data; }

void nvnTextureBuilderSetPackagedTextureLayout(tex_builder_t *b, const void *layout) {
    if (!b) return;
    b->has_layout = layout != NULL;
    if (layout) memcpy(b->layout, layout, 8);
    b->d.pkg_block_h = layout ? (int)((const uint8_t *)layout)[0] + 1 : 0;
}

void nvnTextureBuilderSetStride(tex_builder_t *b, ptrdiff_t s) { if (b) b->d.stride = (uint32_t)s; }

void nvnTextureBuilderSetGLTextureName(tex_builder_t *b, uint32_t n) { (void)b; (void)n; }

size_t nvnTextureBuilderGetStorageSize(const tex_builder_t *b) { nvn_api("nvnTextureBuilderGetStorageSize");
    if (!b) return 0;
    tex_layout L; nvn_host_tex_layout(&b->d, &L);
    if (nvn_trace_save_ui()) {
        extern const char *runtime_fs_last_op(void);
        fprintf(stderr, "[totk-save-ui] StorageSize target=%d fmt=%#x %ux%ux%u lv=%u size=%zu fs=\"%s\"\n",
                b->d.target, b->d.format, b->d.width, b->d.height, b->d.depth,
                b->d.levels, (size_t)L.size, runtime_fs_last_op());
    }
    return (size_t)L.size;
}

size_t nvnTextureBuilderGetStorageAlignment(const tex_builder_t *b) {
    if (!b) return 512;
    tex_layout L; nvn_host_tex_layout(&b->d, &L);
    return L.align;
}

ptrdiff_t nvnTextureBuilderGetViewOffset(const tex_builder_t *b, const uint8_t *v) {
    if (!b || !v) return 0;
    tex_layout L; nvn_host_tex_layout(&b->d, &L);
    int set_levels = v[0] & 1, set_layers = (v[0] >> 1) & 1;
    unsigned base_level = v[1], min_layer = (unsigned)v[4] | ((unsigned)v[5] << 8);
    uint64_t off = set_levels ? nvn_host_tex_level_offset(&b->d, base_level) : 0;
    if (set_layers) off += (uint64_t)min_layer * L.layer_size;
    return (ptrdiff_t)off;
}

const void *nvnTextureBuilderGetPackagedTextureLayout(const tex_builder_t *b) {
    return b && b->has_layout ? b->layout : NULL;
}

int nvnTextureBuilderGetRawStorageClass(const tex_builder_t *b) { (void)b; return 1; }

int nvnTextureBuilderGetStorageClass(const tex_builder_t *b) {
    return b && b->d.target == NVN_TARGET_BUFFER ? 0 : 1;
}

int nvnTextureBuilderGetFlags(const tex_builder_t *b) { return b ? b->d.flags : 0; }

int nvnTextureBuilderGetTarget(const tex_builder_t *b) { return b ? b->d.target : 0; }

int nvnTextureBuilderGetWidth(const tex_builder_t *b) { return b ? (int)b->d.width : 0; }

int nvnTextureBuilderGetHeight(const tex_builder_t *b) { return b ? (int)b->d.height : 0; }

int nvnTextureBuilderGetDepth(const tex_builder_t *b) { return b ? (int)b->d.depth : 0; }

int nvnTextureBuilderGetLevels(const tex_builder_t *b) { return b ? (int)b->d.levels : 0; }

int nvnTextureBuilderGetFormat(const tex_builder_t *b) { return b ? b->d.format : 0; }

int nvnTextureBuilderGetSamples(const tex_builder_t *b) { return b ? b->d.samples : 0; }

void nvnTextureBuilderGetSwizzle(const tex_builder_t *b, int *r, int *g, int *bl, int *a) {
    (void)b; if (r) *r = 2; if (g) *g = 3; if (bl) *bl = 4; if (a) *a = 5;
}

int nvnTextureBuilderGetDepthStencilMode(const tex_builder_t *b) { (void)b; return 0; }

const void *nvnTextureBuilderGetPackagedTextureData(const tex_builder_t *b) { return b ? b->pkg_data : NULL; }

ptrdiff_t nvnTextureBuilderGetStride(const tex_builder_t *b) { return b ? (ptrdiff_t)b->d.stride : 0; }

void nvnTextureBuilderGetSparseTileLayout(const tex_builder_t *b, void *out) { (void)b; if (out) memset(out, 0, 0x28); }

uint32_t nvnTextureBuilderGetGLTextureName(const tex_builder_t *b) { (void)b; return 0; }

size_t nvnTextureBuilderGetZCullStorageSize(const tex_builder_t *b) { (void)b; return 0; }

void *nvnTextureBuilderGetMemoryPool(const tex_builder_t *b) { return b ? b->pool : NULL; }

ptrdiff_t nvnTextureBuilderGetMemoryOffset(const tex_builder_t *b) { return b ? (ptrdiff_t)b->off : 0; }

void *nvnTextureBuilderGetDevice(const tex_builder_t *b) { return b ? b->dev : NULL; }

uint32_t nvnTextureInitialize(tex_t *t, const tex_builder_t *b) { nvn_api("nvnTextureInitialize");
    if (!t || !b) { nvn_fail("nvnTextureInitialize", "null texture/builder"); return 0; }
    memset(t, 0, sizeof *t);
    t->magic = NVN_MAGIC('T', 'E', 'X', 0);
    t->d = b->d;
    t->has_layout = b->has_layout;
    memcpy(t->layout, b->layout, 8);
    t->pool = b->pool;
    t->off = b->off;
    nvn_host_tex_layout(&t->d, &t->L);
    uint64_t size = t->L.size ? t->L.size : 1;
    if (t->pool && t->pool->magic == NVN_MAGIC('P', 'O', 'O', 'L') && t->pool->mem) {
        if (t->off < 0 || (uint64_t)t->off > t->pool->size ||
            size > t->pool->size - (uint64_t)t->off) {
            char d[96]; snprintf(d, sizeof d, "range off=%lld size=%llu pool=%llu",
                                 (long long)t->off, (unsigned long long)size,
                                 (unsigned long long)t->pool->size);
            nvn_fail("nvnTextureInitialize", d);
            t->magic = 0;
            return 0;
        }
        t->mem = t->pool->mem + t->off;
    } else if (t->pool && t->pool->magic != NVN_MAGIC('P', 'O', 'O', 'L')) {
        nvn_fail("nvnTextureInitialize", "invalid memory pool");
        t->magic = 0;
        return 0;
    }
    if (b->pkg_data) {
        if (!t->mem) { nvn_fail("nvnTextureInitialize", "packaged data without mapping"); t->magic = 0; return 0; }
        memcpy(t->mem, b->pkg_data, (size_t)size);
    }
    t->serial = (uint64_t)(uintptr_t)t;
    nvn_tex_registry_add(t);
    return 1;
}

int nvn_texture_store_bridge(tex_t *t, GfxTexture *g) {
    pthread_mutex_lock(&g_tex_bridge_lock);
    tex_bridge_t *b = nvn_tex_bridge(t, 1);
    if (!b) { pthread_mutex_unlock(&g_tex_bridge_lock); gfx_backend_texture_destroy(g); return 0; }
    if (b->gfx) gfx_backend_texture_destroy((GfxTexture *)b->gfx);
    b->gfx = g;
    pthread_mutex_unlock(&g_tex_bridge_lock);
    return 1;
}

/* The guest is the source of truth: a CPU write/flush invalidates the host
 * copy so the next bind re-uploads the updated storage. */
void nvn_texture_bridge_drop(const tex_t *t) {
    pthread_mutex_lock(&g_tex_bridge_lock);
    tex_bridge_t *b = nvn_tex_bridge(t, 0);
    if (!b) { pthread_mutex_unlock(&g_tex_bridge_lock); return; }
    if (b->gfx) gfx_backend_texture_destroy((GfxTexture *)b->gfx);
    b->gfx = NULL;
    b->rt_uploaded = 0;   /* force a rebuild on the next bind */
    pthread_mutex_unlock(&g_tex_bridge_lock);
}

int runtime_nvn_texture_upload_rgba8(void *texture, const uint8_t *pixels,
                                    uint32_t width, uint32_t height) {
    tex_t *t = texture;
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !pixels) return 0;
    GfxTexture *g = gfx_backend_texture_create_rgba8(width, height, pixels);
    if (!g) return 0;
    return nvn_texture_store_bridge(t, g);
}

int nvn_texture_gfx_target(int target) {
    switch (target) {
    case NVN_TARGET_2D:
    case NVN_TARGET_RECTANGLE: return GFX_TEXTURE_2D;
    case NVN_TARGET_3D: return GFX_TEXTURE_3D;
    case NVN_TARGET_CUBEMAP: return GFX_TEXTURE_CUBE;
    case NVN_TARGET_2D_ARRAY: return GFX_TEXTURE_2D_ARRAY;
    case NVN_TARGET_CUBEMAP_ARRAY: return GFX_TEXTURE_CUBE_ARRAY;
    default: return -1;
    }
}

uint32_t nvn_texture_gfx_layers(const tex_desc *d) {
    switch (d->target) {
    case NVN_TARGET_2D:
    case NVN_TARGET_RECTANGLE:
    case NVN_TARGET_3D: return 1;
    case NVN_TARGET_CUBEMAP: return 6;
    case NVN_TARGET_2D_ARRAY:
    case NVN_TARGET_CUBEMAP_ARRAY: return d->depth ? d->depth : 1;
    default: return 0;
    }
}
