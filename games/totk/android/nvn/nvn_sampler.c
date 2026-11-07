/* NVN module: sampler. */
#include "nvn_internal.h"

static samp_bridge_t g_samp_bridges[64];

static unsigned g_samp_bridge_count;

GfxSampler *nvn_sampler_gfx(const samp_t *s) {
    if (!s || s->magic != NVN_MAGIC('S', 'A', 'M', 'P')) return NULL;
    for (unsigned i = 0; i < g_samp_bridge_count; i++)
        if (g_samp_bridges[i].samp == s) return g_samp_bridges[i].gfx;
    GfxSamplerDesc d = {
        .min_filter = s->p.minf, .mag_filter = s->p.magf,
        .wrap_s = s->p.ws, .wrap_t = s->p.wt, .wrap_r = s->p.wr,
        .compare_enable = s->p.cmp_mode == 1, .compare_op = s->p.cmp_func,
        .lod_min = s->p.lod_min, .lod_max = s->p.lod_max, .lod_bias = s->p.lod_bias,
        .max_anisotropy = s->p.aniso,
    };
    GfxSampler *g = gfx_backend_sampler_create(&d);
    if (!g) return NULL;
    if (g_samp_bridge_count < sizeof g_samp_bridges / sizeof g_samp_bridges[0]) {
        g_samp_bridges[g_samp_bridge_count++] = (samp_bridge_t){ s, g };
    } else {
        gfx_backend_sampler_destroy(g);
        return NULL;
    }
    return g;
}

/* ------------------------------------------------------------------ */
/* Sampler                                                             */

void nvnSamplerBuilderSetDefaults(samp_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    /* NVN's sampler defaults (same values as the desktop NVN layer). */
    b->p.minf = 4;                        /* NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR */
    b->p.magf = 1;                        /* NVN_MAG_FILTER_LINEAR */
    b->p.ws = b->p.wt = b->p.wr = 1;      /* NVN_WRAP_MODE_REPEAT */
    b->p.cmp_func = 2;                    /* NVN_COMPARE_FUNC_LESS */
    b->p.lod_min = -1000.f; b->p.lod_max = 1000.f; b->p.aniso = 1.f;
}

void nvnSamplerBuilderSetDevice(samp_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnSamplerBuilderSetMinMagFilter(samp_builder_t *b, int min, int mag) {
    if (b) { b->p.minf = (uint8_t)min; b->p.magf = (uint8_t)mag; }
}

void nvnSamplerBuilderSetWrapMode(samp_builder_t *b, int s, int t, int r) {
    if (b) { b->p.ws = (uint8_t)s; b->p.wt = (uint8_t)t; b->p.wr = (uint8_t)r; }
}

void nvnSamplerBuilderSetLodClamp(samp_builder_t *b, float lo, float hi) {
    if (b) { b->p.lod_min = lo; b->p.lod_max = hi; }
}

void nvnSamplerBuilderSetLodBias(samp_builder_t *b, float v) { if (b) b->p.lod_bias = v; }

void nvnSamplerBuilderSetCompare(samp_builder_t *b, int mode, int func) {
    if (b) { b->p.cmp_mode = (uint8_t)mode; b->p.cmp_func = (uint8_t)func; }
}

void nvnSamplerBuilderSetBorderColor(samp_builder_t *b, const float *c) {
    if (b && c) memcpy(b->p.border, c, sizeof b->p.border);
}

void nvnSamplerBuilderSetBorderColori(samp_builder_t *b, const int *c) {
    if (!b || !c) return;
    for (int i = 0; i < 4; i++) b->p.border[i] = (float)c[i];
}

void nvnSamplerBuilderSetBorderColorui(samp_builder_t *b, const uint32_t *c) {
    if (!b || !c) return;
    for (int i = 0; i < 4; i++) b->p.border[i] = (float)c[i];
}

void nvnSamplerBuilderSetMaxAnisotropy(samp_builder_t *b, float v) { if (b) b->p.aniso = v; }

void nvnSamplerBuilderSetReductionFilter(samp_builder_t *b, int v) { if (b) b->p.reduction = (uint8_t)v; }

void nvnSamplerBuilderSetLodSnap(samp_builder_t *b, float v) { if (b) b->p.lod_snap = v; }

void *nvnSamplerBuilderGetDevice(const samp_builder_t *b) { return b ? b->dev : NULL; }

int nvnSamplerBuilderGetMinMagFilter(const samp_builder_t *b, int *min, int *mag) {
    if (!b || !min || !mag) return 0;
    *min = b->p.minf; *mag = b->p.magf; return 1;
}

int nvnSamplerBuilderGetWrapMode(const samp_builder_t *b, int *s, int *t, int *r) {
    if (!b || !s || !t || !r) return 0;
    *s = b->p.ws; *t = b->p.wt; *r = b->p.wr; return 1;
}

void nvnSamplerBuilderGetLodClamp(const samp_builder_t *b, float *lo, float *hi) {
    if (b && lo && hi) { *lo = b->p.lod_min; *hi = b->p.lod_max; }
}

float nvnSamplerBuilderGetLodBias(const samp_builder_t *b) { return b ? b->p.lod_bias : 0; }

void nvnSamplerBuilderGetCompare(const samp_builder_t *b, int *mode, int *func) {
    if (b && mode && func) { *mode = b->p.cmp_mode; *func = b->p.cmp_func; }
}

void nvnSamplerBuilderGetBorderColor(const samp_builder_t *b, float *c) { if (b && c) memcpy(c, b->p.border, 16); }

void nvnSamplerBuilderGetBorderColori(const samp_builder_t *b, int *c) {
    if (b && c) for (int i = 0; i < 4; i++) c[i] = (int)b->p.border[i];
}

void nvnSamplerBuilderGetBorderColorui(const samp_builder_t *b, uint32_t *c) {
    if (b && c) for (int i = 0; i < 4; i++) c[i] = (uint32_t)b->p.border[i];
}

float nvnSamplerBuilderGetMaxAnisotropy(const samp_builder_t *b) { return b ? b->p.aniso : 0; }

int nvnSamplerBuilderGetReductionFilter(const samp_builder_t *b) { return b ? b->p.reduction : 0; }

float nvnSamplerBuilderGetLodSnap(const samp_builder_t *b) { return b ? b->p.lod_snap : 0; }

uint32_t nvnSamplerInitialize(samp_t *s, const samp_builder_t *b) { nvn_api("nvnSamplerInitialize");
    if (!s || !b) { nvn_fail("nvnSamplerInitialize", "null sampler/builder"); return 0; }
    memset(s, 0, sizeof *s);
    s->magic = NVN_MAGIC('S', 'A', 'M', 'P');
    s->p = b->p;
    return 1;
}

void nvnSamplerFinalize(samp_t *s) {
    if (!s) return;
    for (unsigned i = 0; i < g_samp_bridge_count; i++)
        if (g_samp_bridges[i].samp == s) {
            gfx_backend_sampler_destroy(g_samp_bridges[i].gfx);
            g_samp_bridges[i] = g_samp_bridges[--g_samp_bridge_count];
            break;
        }
    s->magic = 0;
}

void nvnSamplerSetDebugLabel(samp_t *s, const char *l) { (void)s; (void)l; }

int nvnSamplerGetMinMagFilter(const samp_t *s, int *min, int *mag) {
    if (!s || !min || !mag) return 0; *min = s->p.minf; *mag = s->p.magf; return 1;
}

int nvnSamplerGetWrapMode(const samp_t *s, int *x, int *y, int *z) {
    if (!s || !x || !y || !z) return 0; *x = s->p.ws; *y = s->p.wt; *z = s->p.wr; return 1;
}

void nvnSamplerGetLodClamp(const samp_t *s, float *lo, float *hi) {
    if (s && lo && hi) { *lo = s->p.lod_min; *hi = s->p.lod_max; }
}

float nvnSamplerGetLodBias(const samp_t *s) { return s ? s->p.lod_bias : 0; }

void nvnSamplerGetCompare(const samp_t *s, int *mode, int *func) {
    if (s && mode && func) { *mode = s->p.cmp_mode; *func = s->p.cmp_func; }
}

void nvnSamplerGetBorderColor(const samp_t *s, float *c) { if (s && c) memcpy(c, s->p.border, 16); }

void nvnSamplerGetBorderColori(const samp_t *s, int *c) { if (s && c) for (int i = 0; i < 4; i++) c[i] = (int)s->p.border[i]; }

void nvnSamplerGetBorderColorui(const samp_t *s, uint32_t *c) { if (s && c) for (int i = 0; i < 4; i++) c[i] = (uint32_t)s->p.border[i]; }

float nvnSamplerGetMaxAnisotropy(const samp_t *s) { return s ? s->p.aniso : 0; }

int nvnSamplerGetReductionFilter(const samp_t *s) { return s ? s->p.reduction : 0; }

uint32_t nvnSamplerCompare(const samp_t *a, const samp_t *b) {
    return a && b && !memcmp(&a->p, &b->p, sizeof a->p);
}

uint64_t nvnSamplerGetDebugID(const samp_t *s) { return s ? (uint64_t)(uintptr_t)s : 0; }

int samp_pool_valid(const samp_pool_t *p) {
    return p && p->magic == NVN_MAGIC('S', 'P', 'O', 'L') &&
           p->n > 0 && p->n <= 0x100000 && p->off >= 0 && !(p->off & 3);
}

uint32_t nvnSamplerPoolInitialize(samp_pool_t *p, const pool_t *pool, ptrdiff_t off, int n) { nvn_api("nvnSamplerPoolInitialize");
    if (!p || !nvn_obj(pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnSamplerPoolInitialize(pool)")) return 0;
    if (n < 0) { nvn_fail("nvnSamplerPoolInitialize", "negative count"); return 0; }
    uint32_t gen = p->magic == NVN_MAGIC('S', 'P', 'O', 'L') ? p->gen + 1 : 1;
    if (!gen) gen = 1;
    nvn_samp_pool_forget(p);
    p->pool = (pool_t *)pool; p->off = off; p->n = n;
    p->magic = NVN_MAGIC('S', 'P', 'O', 'L');
    p->gen = gen; pool_epoch_set(p);
    return 1;
}

void nvnSamplerPoolFinalize(samp_pool_t *p) {
    if (!p) return;
    nvn_samp_pool_forget(p);
    nvn_pool_forget(p);
}

void nvnSamplerPoolSetDebugLabel(samp_pool_t *p, const char *l) { (void)p; (void)l; }

void nvnSamplerPoolRegisterSampler(const samp_pool_t *p, int id, const samp_t *s) { nvn_api("nvnSamplerPoolRegisterSampler");
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] SamplerPoolRegisterSampler pool=%p id=%d sampler=%p lr=%p\n",
                (const void *)p, id, (const void *)s, __builtin_return_address(0));
    if (!p || p->magic != NVN_MAGIC('S', 'P', 'O', 'L') || !nvn_obj(s, NVN_MAGIC('S', 'A', 'M', 'P'), "nvnSamplerPoolRegisterSampler(sampler)")) return;
    if (id < 0 || (uint32_t)id >= p->n) { nvn_fail("nvnSamplerPoolRegisterSampler", "id out of range"); return; }
    nvn_samp_pool_add(p, id, s);
    /* Real NVN writes an opaque 32-byte sampler descriptor into the pool.
     * The host has no GPU, but the descriptor memory must be defined so the
     * guest never reads stale bytes from its own pool. */
    if (p->pool && p->pool->mem) {
        uint64_t off = (uint64_t)p->off + (uint64_t)(uint32_t)id * 32;
        if (off + 32 <= p->pool->size)
            memset(p->pool->mem + off, 0, 32);
        else
            nvn_fail("nvnSamplerPoolRegisterSampler", "descriptor offset outside pool");
    }
}

void nvnSamplerPoolRegisterSamplerBuilder(const samp_pool_t *p, int id, const samp_builder_t *b) {
    if (!p || !b) { nvn_fail("nvnSamplerPoolRegisterSamplerBuilder", "null pool/builder"); return; }
    if (id < 0 || (uint32_t)id >= p->n) { nvn_fail("nvnSamplerPoolRegisterSamplerBuilder", "id out of range"); return; }
}

void *nvnSamplerPoolGetMemoryPool(const samp_pool_t *p) { return p ? p->pool : NULL; }

int64_t nvnSamplerPoolGetMemoryOffset(const samp_pool_t *p) { return p ? p->off : 0; }

int nvnSamplerPoolGetSize(const samp_pool_t *p) { return p ? (int)p->n : 0; }
