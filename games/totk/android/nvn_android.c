/* Minimal NVN object layer for the Android host.
 *
 * This is a CPU-only implementation: it does not submit work to a GPU, but it
 * owns the NVN object graph with the pointer semantics the guest expects.
 * Builders write their fields into the guest's builder memory, initializers
 * write complete objects, every object carries a magic, and mapping functions
 * return the backing storage.  Unknown object APIs are reported instead of
 * silently succeeding.
 *
 * The device-initialization contract (nvnBootstrapLoader, proc addresses and
 * the X1 device-info table) is the same as before: the 1.0.0 boot path checks
 * nvnDeviceGetInteger(0/1) == 55/15 before it creates the SEAD graphics device.
 */
#include "runtime.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "gfx_backend_android.h"
#include "shader_cache_android.h"

typedef uint32_t NvnBool;
typedef void *NvnNativeWindow;
extern int runtime_native_window_present(void *p);
extern void runtime_native_window_destroy(void *p);
extern void *runtime_native_window_create(uint32_t width, uint32_t height,
                                          uint32_t format, uint64_t layer_id);
extern int runtime_native_window_info(const void *p, uint32_t *width, uint32_t *height,
                                      uint32_t *format, void **buffer, size_t *size);
extern int runtime_native_window_valid(const void *p);
extern int runtime_native_window_draw_triangle(void *p);
extern int runtime_native_window_draw_triangle_pipeline(void *p, void *pipeline);
extern int runtime_native_window_draw_textured_triangle(void *p, void *pipeline, void *texture);
extern int runtime_native_window_draw_reflected_pipeline(void *p, void *pipeline);
extern int runtime_native_window_execute_reflected(void *p, void *pipeline,
                                                   const GfxCmd *cmds, uint32_t count,
                                                   const float default_clear[4]);
extern int runtime_native_window_execute_reflected_ex(void *p, void *pipeline,
                                                      const GfxCmd *cmds, uint32_t count,
                                                      const float default_clear[4],
                                                      const GfxVertexStream *streams,
                                                      uint32_t nstreams);
extern int runtime_native_window_clear_rgba(void *p, const float rgba[4]);
extern int runtime_native_window_read_pixel(void *p, uint32_t x, uint32_t y, uint8_t out[4]);
extern int runtime_native_window_screenshot(void *p, const char *path);
static void nvn_process_submit_commands(const uint64_t *cmds, int n);
static int nvn_gpu_drain(const char *what);
static void nvn_host_rt_clear(void);

#define NVN_MAGIC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* Reserved object sizes from the NVN 55 headers (include/nvn.h). */
enum {
    NVN_QUEUE_SIZE             = 0x2000,
    NVN_DEVICE_SIZE            = 0x3000,
    NVN_WINDOW_SIZE            = 0x180,
    NVN_MEMORY_POOL_SIZE       = 0x100,
    NVN_TEXTURE_SIZE           = 0x80,
    NVN_TEXTURE_VIEW_SIZE      = 0x10,
    NVN_COMMAND_BUFFER_SIZE    = 0xA0,
    NVN_PROGRAM_SIZE           = 0xC0,
    NVN_SYNC_SIZE              = 0x40,
    NVN_EVENT_SIZE             = 0x40,
    NVN_SAMPLER_SIZE           = 0x30,
    NVN_BUFFER_SIZE            = 0x30,
    NVN_TEXTURE_POOL_SIZE      = 0x20,
    NVN_SAMPLER_POOL_SIZE      = 0x20,
};

/* NVN enums we need. */
enum {
    NVN_TARGET_1D = 0,
    NVN_TARGET_2D = 1,
    NVN_TARGET_3D = 2,
    NVN_TARGET_1D_ARRAY = 3,
    NVN_TARGET_2D_ARRAY = 4,
    NVN_TARGET_2D_MS = 5,
    NVN_TARGET_2D_MS_ARRAY = 6,
    NVN_TARGET_CUBEMAP = 8,
    NVN_TARGET_CUBEMAP_ARRAY = 9,
    NVN_TARGET_BUFFER = 10,
    NVN_TARGET_RECTANGLE = 7,
};

enum {
    NVN_POOL_CPU_NO_ACCESS = 0x1,
    NVN_POOL_GPU_NO_ACCESS = 0x8,
    NVN_POOL_PHYSICAL = 0x100,
    NVN_POOL_VIRTUAL = 0x200,
};

enum {
    NVN_SYNC_WAIT_ALREADY_SIGNALED = 0,
    NVN_SYNC_WAIT_CONDITION_SATISFIED = 1,
    NVN_SYNC_WAIT_TIMEOUT_EXPIRED = 2,
    NVN_SYNC_WAIT_FAILED = 3,
};

enum {
    NVN_EVENT_SIGNAL_MODE_WRITE = 0,
    NVN_EVENT_SIGNAL_MODE_ADD = 1,
};

enum {
    NVN_DRAW_POINTS = 0,
    NVN_DRAW_LINES = 1,
    NVN_DRAW_LINE_LOOP = 2,
    NVN_DRAW_LINE_STRIP = 3,
    NVN_DRAW_TRIANGLES = 4,
    NVN_DRAW_TRIANGLE_STRIP = 5,
    NVN_DRAW_TRIANGLE_FAN = 6,
};

/* Command-buffer translation records the guest order first (clear, viewport,
 * scissor, draw).  Descriptor/resource state is bound through the reflected
 * pipeline's sets; vertex/index resource reads are the next step. */
enum {
    NVN_CMD_CLEAR_COLOR = 1,
    NVN_CMD_CLEAR_DEPTH_STENCIL,
    NVN_CMD_SET_VIEWPORT,
    NVN_CMD_SET_SCISSOR,
    NVN_CMD_SET_RENDER_TARGETS,
    NVN_CMD_BIND_DESC,
    NVN_CMD_BIND_STATE,
    NVN_CMD_DRAW,
    NVN_CMD_DRAW_INDEXED,
};

/* Packed NVN state objects (same layout as the macOS NVN layer). */
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } nvn_color_st;
typedef struct { uint32_t mask; } nvn_chmask_st;
typedef struct {
    uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7,
             eq_rgb : 3, eq_a : 3;
} nvn_blend_st;
typedef struct {
    uint8_t enable, samples, a2c, a2c_dither, raster_samples,
            cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid;
} nvn_ms_st;
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } nvn_poly_st;
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4,
             fail_b : 4, zfail_b : 4, zpass_b : 4;
} nvn_ds_st;

typedef struct {
    nvn_blend_st blend;
    nvn_color_st color;
    nvn_chmask_st chmask;
    nvn_ms_st ms;
    nvn_poly_st poly;
    nvn_ds_st ds;
    float blend_color[4];
    int stencil_ref;
    int stencil_mask;
    int stencil_value_mask;
} nvn_render_state_t;

static void nvn_render_state_defaults(nvn_render_state_t *r) {
    memset(r, 0, sizeof *r);
    r->blend = (nvn_blend_st){ 0, 2, 1, 2, 1, 1, 1 };  /* ONE/ZERO, ADD */
    r->color = (nvn_color_st){ 0, 3, 8 };               /* COPY, ALWAYS */
    r->chmask.mask = 0xffffffffu;
    r->ms = (nvn_ms_st){ .enable = 1, .samples = 1 };
    r->poly = (nvn_poly_st){ .cull = 0, .front = 1, .mode = 2 };
    r->ds = (nvn_ds_st){ .dt = 0, .dw = 1, .df = 2, .st = 0,
                         .sf_f = 8, .sf_b = 8,
                         .fail_f = 1, .zfail_f = 1, .zpass_f = 1,
                         .fail_b = 1, .zfail_b = 1, .zpass_b = 1 };
}

typedef struct {
    uint32_t kind;
    union {
        struct { float rgba[4]; int index; uint32_t mask; } clear;
        struct { float depth; uint32_t dmask; int stencil; uint32_t smask; } clear_ds;
        struct { int32_t x, y, width, height; } rect;
        /* Complete NVN render-target set: every color target survives to the
         * submit path, plus the depth target. */
        struct {
            uint32_t count;
            void *tex[GFX_MAX_COLOR_TARGETS];
            void *depth;
        } targets;
        struct { uint32_t stage, kind, slot; uint64_t address; size_t size; uint64_t handle; } desc;
        struct { nvn_render_state_t rs; } state;
        struct { uint32_t primitive, first, count, instances, base_instance; } draw;
        struct { uint32_t primitive, count, index_type, base_vertex; uint64_t indices; } draw_indexed;
    } u;
} nvn_cmd_t;

/* Last bound descriptor state of a command buffer.  NVN binds by stage/slot;
 * the reflected pipeline maps that to a SPIR-V binding through .res.json. */
typedef struct {
    struct { uint64_t address; size_t size; } ubo[6][16];
    struct { uint64_t address; size_t size; } ssbo[6][16];
    uint64_t tex[6][128];
    uint32_t ubo_mask[6], ssbo_mask[6];
    uint32_t tex_mask[6][4];
    uint32_t va[16];
    uint64_t vs[16];
    struct { uint64_t address; size_t size; } vb[16];
    uint32_t va_mask, vs_mask, vb_mask;
    nvn_render_state_t rs;
    const void *depth_tex;
} nvn_bind_state_t;

static const char *g_last_api = "none";
static char g_last_proc[80] = "none";
static int nvn_trace(void);
static void nvn_api(const char *what) {
    g_last_api = what;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] api %s thread=%#x\n", what, runtime_thread_current_handle());
}
const char *runtime_nvn_last_api(void) { return g_last_api; }
const char *runtime_nvn_last_proc(void) { return g_last_proc; }

static uint32_t nvn_fail_count;
static void nvn_fail(const char *what, const char *detail) {
    if (nvn_fail_count++ < 48)
        fprintf(stderr, "[totk-nvn] ERROR %s%s%s\n", what,
                detail && *detail ? ": " : "", detail ? detail : "");
}
static int nvn_trace(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_ANDROID_TRACE_NVN") != NULL ||
                   getenv("TOTK_ANDROID_TRACE_FRAME") != NULL;
    return on;
}

static int nvn_obj(const void *p, uint32_t magic, const char *what) {
    if (!p) { nvn_fail(what, "null object"); return 0; }
    if (*(const uint32_t *)p != magic) {
        char d[80];
        snprintf(d, sizeof d, "bad magic %#x (expected %#x)",
                 *(const uint32_t *)p, magic);
        nvn_fail(what, d);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Object layouts (all fit the NVN reserved sizes above)               */

typedef struct { uint32_t flags; } dev_builder_t;

typedef struct {
    uint32_t magic, flags, origin, depth_mode;
    void *dbg_cb, *dbg_data;
} nvn_dev_t;

typedef struct {
    void *dev;
    uint32_t flags, priority;
    uint64_t cmd_sz, comp_sz, ctl_sz, flush;
    void *mem;
    uint64_t mem_sz;
} queue_builder_t;

typedef struct {
    uint32_t magic;
    void *dev;
    uint64_t submits, cmd_used, comp_used, ctl_used;
} queue_t;

typedef struct {
    void *dev;
    void *native;
    int32_t ntex, interval, active, pad;
    void *tex[4];
} win_builder_t;

typedef struct {
    uint32_t magic, pad;
    void *native;
    int32_t ntex, interval, next, active;
    int32_t crop[4];
    void *tex[4];
    uint64_t presents;
    int owned_native;
} win_t;

typedef struct {
    void *dev;
    void *mem;
    uint64_t size;
    uint32_t flags, pad;
} pool_builder_t;

typedef struct {
    uint32_t magic, flags;
    uint8_t *mem;
    uint64_t size;
    void *dev;
    uint32_t map_count, owned;
    uint64_t host_size;
} pool_t;

typedef struct {
    void *dev;
    void *pool;
    int64_t off;
    uint64_t size;
} buf_builder_t;

typedef struct {
    uint32_t magic, pad;
    pool_t *pool;
    int64_t off;
    uint64_t size;
} buf_t;

typedef struct {
    int target, format, flags, samples;
    uint32_t width, height, depth, levels, stride;
    int pkg_block_h;
} tex_desc;

typedef struct {
    uint64_t size, layer_size;
    uint32_t align, layers, bpb;
    unsigned tileH, tileD;
} tex_layout;

typedef struct {
    void *dev;
    tex_desc d;
    void *pool;
    int64_t off;
    const void *pkg_data;
    uint8_t has_layout, layout[8];
    uint8_t pad[7];
} tex_builder_t;

typedef struct {
    uint32_t magic;
    tex_desc d;
    uint8_t has_layout, layout[8];
    tex_layout L;
    uint8_t *mem;
    pool_t *pool;
    int64_t off;
    uint64_t serial;
} tex_t;

/* Live NVN texture objects.  The window path needs the guest texture's
 * dimensions/format, but a guest-provided pointer must never be dereferenced
 * unless nvnTextureInitialize produced it: the registry gives us a safe
 * lookup (some callers pass small integers as placeholder textures). */
static tex_t **g_tex_objs;
static unsigned g_tex_obj_count, g_tex_obj_cap;
static pthread_mutex_t g_tex_obj_lock = PTHREAD_MUTEX_INITIALIZER;
static void nvn_tex_registry_add(tex_t *t) {
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
static void nvn_tex_registry_del(tex_t *t) {
    pthread_mutex_lock(&g_tex_obj_lock);
    for (unsigned i = 0; i < g_tex_obj_count; i++) {
        if (g_tex_objs[i] != t) continue;
        g_tex_objs[i] = g_tex_objs[--g_tex_obj_count];
        break;
    }
    pthread_mutex_unlock(&g_tex_obj_lock);
}
static const tex_t *nvn_tex_registry_find(const void *p) {
    const tex_t *found = NULL;
    if (!p) return NULL;
    pthread_mutex_lock(&g_tex_obj_lock);
    for (unsigned i = 0; i < g_tex_obj_count; i++)
        if (g_tex_objs[i] == p) { found = g_tex_objs[i]; break; }
    pthread_mutex_unlock(&g_tex_obj_lock);
    return found;
}

typedef struct {
    uint32_t magic, n;
    pool_t *pool;
    int64_t off;
} tex_pool_t;

/* Host bridge for the 2B.3 texture path; kept out of tex_t so the guest's
 * NVN_TEXTURE_SIZE contract is unchanged. */
typedef struct { const tex_t *tex; void *gfx; } tex_bridge_t;
static tex_bridge_t g_tex_bridges[64];
static unsigned g_tex_bridge_count;
typedef struct {
    uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction;
    float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap;
} samp_par;

typedef struct { void *dev; samp_par p; } samp_builder_t;
typedef struct { uint32_t magic; samp_par p; } samp_t;
typedef struct { const samp_t *samp; GfxSampler *gfx; } samp_bridge_t;
static samp_bridge_t g_samp_bridges[64];
static unsigned g_samp_bridge_count;
typedef struct { uint32_t magic, n; pool_t *pool; int64_t off; } samp_pool_t;

/* NVN texture/sampler pool registrations.  The pool objects themselves are
 * guest-sized (0x20 bytes), so the id -> object map lives on the host. */
typedef struct { const tex_pool_t *pool; int id; const tex_t *tex; } tex_pool_map_t;
typedef struct { const samp_pool_t *pool; int id; const samp_t *samp; } samp_pool_map_t;
/* The boot scene registers more than a thousand textures (810 by the first
 * NVN submit in the 1.0.0 boot path) plus samplers, so the old 256-entry
 * tables silently dropped registrations and every later BindTexture resolved
 * to NULL.  Keep enough room for a full scene and report overflow instead of
 * hiding it. */
#define NVN_POOL_MAP_MAX 16384
static tex_pool_map_t g_tex_pool_map[NVN_POOL_MAP_MAX];
static unsigned g_tex_pool_map_count;
static samp_pool_map_t g_samp_pool_map[NVN_POOL_MAP_MAX];
static unsigned g_samp_pool_map_count;

typedef struct {
    uint32_t magic, nstage;
    uint64_t data[6];
    const void *control[6];
    void *gfx_pipeline;       /* host Vulkan pipeline for the 2B.3 draw path */
    void *gfx_textured_pipeline;
    void *gfx_texture;
    void *gfx_reflected_pipeline;  /* full .res.json descriptor layout */
    void *shader_state;       /* obsolete: shader state lives in prog_state map */
    uint32_t textured;
    uint64_t serial;
} prog_t;

typedef struct {
    uint32_t magic, state;    /* 0 pending, 1 signaled (GPU completion seen) */
    void *fence;              /* GfxFence* from the backend contract */
    uint64_t fence_generation;
    uint64_t submit_generation;
    int32_t condition;        /* NVNsyncCondition recorded by QueueFenceSync */
    uint32_t pad;
} sync_t;
static uint32_t nvnSyncInitialize(sync_t *s, const nvn_dev_t *d);
static void nvnSyncFinalize(sync_t *s);
typedef struct { pool_t *pool; int64_t off; } ev_builder_t;
typedef struct { uint32_t magic, pad; volatile uint32_t *val; pool_t *pool; int64_t off; } ev_t;

/* The guest allocates the NVN reserved sizes; the host layouts must fit. */
_Static_assert(sizeof(queue_t) <= NVN_QUEUE_SIZE, "queue object too large");
_Static_assert(sizeof(win_t) <= NVN_WINDOW_SIZE, "window object too large");
_Static_assert(sizeof(pool_t) <= NVN_MEMORY_POOL_SIZE, "memory pool object too large");
_Static_assert(sizeof(tex_t) <= NVN_TEXTURE_SIZE, "texture object too large");
_Static_assert(sizeof(samp_t) <= NVN_SAMPLER_SIZE, "sampler object too large");
_Static_assert(sizeof(buf_t) <= NVN_BUFFER_SIZE, "buffer object too large");
_Static_assert(sizeof(prog_t) <= NVN_PROGRAM_SIZE, "program object too large");
_Static_assert(sizeof(sync_t) <= NVN_SYNC_SIZE, "sync object too large");
_Static_assert(sizeof(ev_t) <= NVN_EVENT_SIZE, "event object too large");
_Static_assert(sizeof(tex_pool_t) <= NVN_TEXTURE_POOL_SIZE, "texture pool object too large");
_Static_assert(sizeof(samp_pool_t) <= NVN_SAMPLER_POOL_SIZE, "sampler pool object too large");
_Static_assert(sizeof(tex_builder_t) <= 0x80, "texture builder too large");
_Static_assert(sizeof(samp_builder_t) <= 0x60, "sampler builder too large");
_Static_assert(sizeof(queue_builder_t) <= 0x40, "queue builder too large");
_Static_assert(sizeof(win_builder_t) <= 0x40, "window builder too large");
_Static_assert(sizeof(pool_builder_t) <= 0x40, "pool builder too large");
_Static_assert(sizeof(buf_builder_t) <= 0x40, "buffer builder too large");
_Static_assert(sizeof(dev_builder_t) <= 0x40, "device builder too large");
_Static_assert(sizeof(ev_builder_t) <= 0x20, "event builder too large");

/* Texture storage layout (Tegra X1 block-linear).  The guest uses these
 * values to size and offset memory pools, so the calculation is kept
 * consistent even though nothing is submitted to a GPU. */
typedef struct { uint8_t bpb, bw, bh; } nvn_fmt_t;

static nvn_fmt_t nvn_fmt_of(int f) {
    static const uint8_t astc[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
                                         {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; return (nvn_fmt_t){16, astc[k][0], astc[k][1]}; }
    if (f >= 0x41 && f <= 0x50) {
        int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) || (f == 0x49 || f == 0x4A);
        return (nvn_fmt_t){b8 ? 8 : 16, 4, 4};
    }
    static const struct { uint8_t lo, hi, bpb; } r[] = {
        {0x01,0x04,1},{0x05,0x09,2},{0x0A,0x0C,4},{0x0D,0x10,2},{0x11,0x15,4},{0x16,0x18,8},
        {0x19,0x1C,3},{0x1D,0x21,6},{0x22,0x24,12},{0x25,0x28,4},{0x29,0x2D,8},{0x2E,0x30,16},
        {0x31,0x31,1},{0x32,0x32,2},{0x33,0x35,4},{0x36,0x36,8},{0x37,0x38,4},{0x39,0x3C,2},{0x3D,0x40,4},
        {0x51,0x52,1},{0x53,0x54,2},{0x55,0x56,4},{0x57,0x58,2},{0x59,0x5A,4},{0x5B,0x5C,8},
        {0x5D,0x5E,3},{0x5F,0x60,6},{0x61,0x62,12},{0x63,0x64,4},{0x65,0x66,8},{0x67,0x68,16},
        {0x69,0x6C,4},{0x6D,0x70,4},{0x71,0x75,8},{0x76,0x78,16},{0x95,0x98,2},{0x99,0x9C,4},
    };
    for (unsigned i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) return (nvn_fmt_t){r[i].bpb, 1, 1};
    return (nvn_fmt_t){0, 1, 1};
}
static uint32_t nvn_mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint32_t nvn_blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }
static unsigned nvn_pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }
static unsigned nvn_adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}
static void nvn_shape(const tex_desc *d, unsigned *dpl, int *layered,
                      uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = d->width ? d->width : 1;
    *h = d->height ? d->height : 1;
    *z = d->depth ? d->depth : 1;
    *layered = 0;
    switch (d->target) {
    case NVN_TARGET_1D: *dpl = 1; *h = 1; *z = 1; break;
    case NVN_TARGET_1D_ARRAY: *dpl = 1; *z = d->height ? d->height : 1; *h = 1; *layered = 1; break;
    case NVN_TARGET_3D: *dpl = 3; break;
    case NVN_TARGET_2D_ARRAY: case NVN_TARGET_2D_MS_ARRAY: *dpl = 2; *layered = 1; break;
    case NVN_TARGET_CUBEMAP: *dpl = 2; *z = 6; *layered = 1; break;
    case NVN_TARGET_CUBEMAP_ARRAY: *dpl = 2; *layered = 1; break;
    default: *dpl = 2; *z = 1; break;
    }
    if (*z == 0) *z = 1;
}
static void nvn_samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2;
    else if (s == 4) *sx = *sy = 2;
    else if (s == 8) { *sx = 4; *sy = 2; }
}
static int nvn_tex_linear(const tex_desc *d) { return (d->flags & 0x50) != 0; }
static uint32_t nvn_linear_stride(const tex_desc *d) {
    if (d->stride) return d->stride;
    int bw, bh; int bpb = nvn_fmt_of(d->format).bpb; (void)bw; (void)bh;
    uint32_t align = (d->flags & 0x40) ? 128 : 32;
    return ((((d->width ? d->width : 1) + nvn_fmt_of(d->format).bw - 1) /
             nvn_fmt_of(d->format).bw) * bpb + align - 1) & ~(align - 1);
}
static uint64_t nvn_level_offset(const tex_desc *d, unsigned level, unsigned dpl,
                                 uint32_t w, uint32_t h, uint32_t z,
                                 unsigned tileH, unsigned tileD, nvn_fmt_t t) {
    uint32_t sx, sy; nvn_samples_xy(d->samples, &sx, &sy);
    uint64_t off = 0;
    for (unsigned i = 0; i < level; i++) {
        uint32_t lw = nvn_blk(nvn_mip(w * sx, i), t.bw);
        uint32_t lh = dpl >= 2 ? nvn_blk(nvn_mip(h * sy, i), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? nvn_mip(z, i) : 1;
        uint32_t lwb = lw * t.bpb;
        unsigned th = nvn_adj_tile(tileH, 8, lh), td = nvn_adj_tile(tileD, 1, ld);
        uint32_t wg = (lwb + 63) / 64, hg = (lh + 7) / 8;
        uint32_t wt = wg, ht = (hg + (1u << th) - 1) >> th, dt = (ld + (1u << td) - 1) >> td;
        off += (uint64_t)wt * ht * dt << (9 + th + td);
    }
    return off;
}
static uint64_t nvn_layer_bytes(const tex_desc *d, unsigned dpl, int layered,
                                uint32_t w, uint32_t h, uint32_t z,
                                unsigned tileH, unsigned tileD, nvn_fmt_t t) {
    uint64_t ls = nvn_level_offset(d, d->levels ? d->levels : 1, dpl, w, h, z, tileH, tileD, t);
    if (layered) {
        uint32_t hh = nvn_blk(h, t.bh); unsigned th = tileH, td = tileD;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        uint64_t align = 1ull << (th + td + 9);
        ls = (ls + align - 1) & ~(align - 1);
    }
    return ls;
}
static void nvn_tex_layout(const tex_desc *d, tex_layout *o) {
    nvn_fmt_t t = nvn_fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    nvn_shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy; nvn_samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb; o->tileH = o->tileD = 0; o->layers = layered ? z : 1;
    if (!t.bpb) { o->size = o->layer_size = 0; o->align = 512; return; }
    if (d->target == NVN_TARGET_BUFFER) {
        o->layer_size = o->size = (uint64_t)t.bpb * (d->width ? d->width : 1);
        o->align = t.bpb == 12 ? 4 : t.bpb;
        return;
    }
    if (nvn_tex_linear(d)) {
        o->align = (d->flags & 0x40) ? 128 : 32;
        o->layer_size = o->size = (uint64_t)nvn_linear_stride(d) * h;
        return;
    }
    unsigned tileH = 0, tileD = 0;
    if (d->pkg_block_h) {
        unsigned pl = (unsigned)(d->pkg_block_h - 1) & 0xFF;
        if (dpl >= 2) tileH = pl & 0xF;
        if (dpl == 3) tileD = pl >> 4;
    } else if (dpl == 2) {
        uint32_t ah = nvn_blk(h * sy, t.bh);
        tileH = nvn_pick_tile((ah + ah / 2) / 8);
    } else if (dpl == 3) {
        tileD = nvn_pick_tile(z + z / 2);
    }
    uint64_t ls = nvn_layer_bytes(d, dpl, layered, w, h, z, tileH, tileD, t);
    /* NVN v55's MINIMAL_LAYOUT path: when the resource asks for it and the
     * one-GOB footprint differs from the default, the driver uses tile H=D=0.
     * Omitting this over-allocates the game's texture heaps. */
    static int minimal = -1;
    if (minimal < 0) {
        const char *e = getenv("TOTK_MINIMAL_LAYOUT");
        minimal = !(e && !strcmp(e, "0"));
    }
    if ((d->flags & 0x200) && !d->pkg_block_h && minimal) {
        uint64_t one_gob = nvn_layer_bytes(d, dpl, layered, w, h, z, 0, 0, t);
        if (one_gob != ls) { tileH = tileD = 0; ls = one_gob; }
    }
    o->tileH = tileH; o->tileD = tileD;
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = 512;
}
static uint64_t nvn_tex_level_offset(const tex_desc *d, unsigned level) {
    nvn_fmt_t t = nvn_fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    nvn_shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || nvn_tex_linear(d) || d->target == NVN_TARGET_BUFFER) return 0;
    tex_layout L; nvn_tex_layout(d, &L);
    return nvn_level_offset(d, level, dpl, w, h, z, L.tileH, L.tileD, t);
}

/* ------------------------------------------------------------------ */
/* Device                                                              */

static const int nvn_devinfo[] = {
    [0x00] = 55, [0x01] = 15, [0x02] = 14, [0x03] = 65536, [0x04] = 256,
    [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16,
    [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4,
    [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256,
    [0x19] = 4096, [0x1A] = 131072, [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024,
    [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 262144, [0x2C] = 15,
    [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5,
    [0x32] = 1, [0x33] = 8, [0x34] = 8, [0x35] = 4, [0x36] = 32,
    [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1,
    [0x3C] = 4, [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048,
    [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1,
    [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16, [0x4E] = 1024,
    [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536,
    [0x53] = 256, [0x54] = 16384, [0x55] = 262144, [0x56] = 4096,
    [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096,
    [0x5C] = 16384, [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
    [0x60] = 1, [0x61] = 1,
};

static void nvnDeviceBuilderSetDefaults(dev_builder_t *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
}
static void nvnDeviceBuilderSetFlags(dev_builder_t *b, int flags) {
    if (b) b->flags = (uint32_t)flags;
}
static uint32_t nvnDeviceBuilderGetFlags(const dev_builder_t *b) {
    return b ? b->flags : 0;
}

static uint32_t nvnDeviceInitialize(nvn_dev_t *d, const dev_builder_t *b) { nvn_api("nvnDeviceInitialize");
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->magic = NVN_MAGIC('D', 'E', 'V', 0);
    d->flags = b ? b->flags : 0;
    d->origin = 0;
    d->depth_mode = 0;
    fprintf(stderr, "[totk-nvn] device initialized flags=%#x (CPU-only, no GPU submit)\n", d->flags);
    return 1;
}
static void nvnDeviceFinalize(nvn_dev_t *d) { if (d) { nvn_host_rt_clear(); d->magic = 0; } }
static void nvnDeviceSetDebugLabel(nvn_dev_t *d, const char *label) { (void)d; (void)label; }
static void nvnDeviceGetInteger(const nvn_dev_t *d, int what, int *out) { nvn_api("nvnDeviceGetInteger");
    if (!d || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceGetInteger") || !out) return;
    if (what >= 0 && (size_t)what < sizeof nvn_devinfo / sizeof *nvn_devinfo) *out = nvn_devinfo[what];
    else *out = 0;
}
static uint64_t nvnDeviceGetCurrentTimestampInNanoseconds(const nvn_dev_t *d) {
    (void)d;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t nvnDeviceGetTimestampInNanoseconds(const nvn_dev_t *d, const void *counter) {
    (void)d; (void)counter;
    return 0;
}
/* NVN texture handles pack the texture pool id in the low 20 bits and the
 * sampler pool id in bits 20..31 (see TextureHandle.Unpack* in the translator).
 * Separate handles are the raw pool ids; the combined handle below is what the
 * game writes into the shader's c[2] handle slots. */
static uint64_t nvnDeviceGetTextureHandle(const nvn_dev_t *d, int index, int sampler) {
    (void)d;
    return ((uint64_t)((uint32_t)sampler & 0xfffu) << 20) | ((uint32_t)index & 0xfffffu);
}
static uint64_t nvnDeviceGetTexelFetchHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}
static uint64_t nvnDeviceGetImageHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}
static uint64_t nvnDeviceGetSeparateTextureHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}
static uint64_t nvnDeviceGetSeparateSamplerHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffu;
}
static uint32_t nvnDeviceIsExternalDebuggerAttached(const nvn_dev_t *d) { (void)d; return 0; }
static void nvnDeviceInstallDebugCallback(nvn_dev_t *d, const void *cb, void *data, uint32_t enable) {
    if (!d) return;
    d->dbg_cb = enable ? (void *)cb : NULL;
    d->dbg_data = data;
}
static int nvnDeviceGenerateDebugDomainId(const nvn_dev_t *d, const char *name) {
    (void)d; (void)name; return 1;
}
static void nvnDeviceSetWindowOriginMode(nvn_dev_t *d, int mode) {
    if (d && nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceSetWindowOriginMode")) d->origin = mode;
}
static int nvnDeviceGetWindowOriginMode(const nvn_dev_t *d) { return d ? d->origin : 0; }
static void nvnDeviceSetDepthMode(nvn_dev_t *d, int mode) {
    if (d && nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceSetDepthMode")) d->depth_mode = mode;
}
static int nvnDeviceGetDepthMode(const nvn_dev_t *d) { return d ? d->depth_mode : 0; }
static uint32_t nvnDeviceRegisterFastClearColor(nvn_dev_t *d, const float *c, int f) { (void)d; (void)c; (void)f; return 1; }
static uint32_t nvnDeviceRegisterFastClearColori(nvn_dev_t *d, const int *c, int f) { (void)d; (void)c; (void)f; return 1; }
static uint32_t nvnDeviceRegisterFastClearColorui(nvn_dev_t *d, const uint32_t *c, int f) { (void)d; (void)c; (void)f; return 1; }
static uint32_t nvnDeviceRegisterFastClearDepth(nvn_dev_t *d, float v) { (void)d; (void)v; return 1; }
static void nvnDeviceSetIntermediateShaderCache(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceApplyDeferredFinalizes(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceFinalizeCommandHandle(nvn_dev_t *d, uint64_t h) { (void)d; (void)h; }
static void nvnDeviceWalkDebugDatabase(const nvn_dev_t *d, int t, void *cb, void *u) { (void)d; (void)t; (void)cb; (void)u; }
static void nvnDeviceSetAutomaticCheckpointsEnable(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceSetAutomaticCheckpointsMask(nvn_dev_t *d, uint64_t v) { (void)d; (void)v; }
static void nvnDeviceSetAutomaticCheckpointsSamplingInterval(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceSetAutomaticCheckpointsByDebugGroup(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceSetCheckpointCallStacksEnable(nvn_dev_t *d, int v) { (void)d; (void)v; }
static void nvnDeviceSetCheckpointMaxCallStacksDepth(nvn_dev_t *d, int v) { (void)d; (void)v; }

/* ------------------------------------------------------------------ */
/* Queue                                                               */

static void nvnQueueBuilderSetDefaults(queue_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->cmd_sz = 65536; b->comp_sz = 262144; b->ctl_sz = 16384; b->flush = 4096;
}
static void nvnQueueBuilderSetDevice(queue_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnQueueBuilderSetFlags(queue_builder_t *b, int f) { if (b) b->flags = (uint32_t)f; }
static void nvnQueueBuilderSetCommandMemorySize(queue_builder_t *b, size_t n) { if (b) b->cmd_sz = n; }
static void nvnQueueBuilderSetComputeMemorySize(queue_builder_t *b, size_t n) { if (b) b->comp_sz = n; }
static void nvnQueueBuilderSetControlMemorySize(queue_builder_t *b, size_t n) { if (b) b->ctl_sz = n; }
static void nvnQueueBuilderSetCommandFlushThreshold(queue_builder_t *b, size_t n) { if (b) b->flush = n; }
static void nvnQueueBuilderSetQueuePriority(queue_builder_t *b, int p) { if (b) b->priority = (uint32_t)p; }
static int nvnQueueBuilderGetQueuePriority(const queue_builder_t *b) { return b ? (int)b->priority : 0; }
static size_t nvnQueueBuilderGetQueueMemorySize(const queue_builder_t *b) {
    if (!b) return 0;
    return (size_t)((b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~(uint64_t)4095);
}
static void nvnQueueBuilderSetQueueMemory(queue_builder_t *b, void *m, size_t n) {
    if (b) { b->mem = m; b->mem_sz = n; }
}
static void *nvnQueueBuilderGetMemory(const queue_builder_t *b) { return b ? b->mem : NULL; }
static size_t nvnQueueBuilderGetMemorySize(const queue_builder_t *b) { return b ? (size_t)b->mem_sz : 0; }
static void *nvnQueueBuilderGetDevice(const queue_builder_t *b) { return b ? b->dev : NULL; }
static int nvnQueueBuilderGetFlags(const queue_builder_t *b) { return b ? (int)b->flags : 0; }
static size_t nvnQueueBuilderGetCommandMemorySize(const queue_builder_t *b) { return b ? (size_t)b->cmd_sz : 0; }
static size_t nvnQueueBuilderGetComputeMemorySize(const queue_builder_t *b) { return b ? (size_t)b->comp_sz : 0; }
static size_t nvnQueueBuilderGetControlMemorySize(const queue_builder_t *b) { return b ? (size_t)b->ctl_sz : 0; }
static size_t nvnQueueBuilderGetCommandFlushThreshold(const queue_builder_t *b) { return b ? (size_t)b->flush : 0; }

static uint32_t nvnQueueInitialize(queue_t *q, const queue_builder_t *b) { nvn_api("nvnQueueInitialize");
    if (!q || !b) { nvn_fail("nvnQueueInitialize", "null queue/builder"); return 0; }
    memset(q, 0, sizeof *q);
    q->magic = NVN_MAGIC('Q', 'U', 'E', 'U');
    q->dev = b->dev;
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] QueueInitialize %p dev=%p memory=%p+%#llx\n",
                (void *)q, b->dev, b->mem, (unsigned long long)b->mem_sz);
    return 1;
}
static void nvnQueueFinalize(queue_t *q) { if (q) q->magic = 0; }
static void nvnQueueSetDebugLabel(queue_t *q, const char *l) { (void)q; (void)l; }
static unsigned nvn_gpu_submits, nvn_gpu_presents;

/* Explicit CPU/GPU ordering.  The backend submits and waits synchronously, so
 * the moment a submit/present returns the GPU has finished; nvn_gpu_drain
 * makes that a checked precondition instead of an accident, and every CPU
 * reader (buffer/pool map, fence wait) goes through it. */
static int nvn_gpu_pending_work;
static int nvn_gpu_drain(const char *what) {
    if (!nvn_gpu_pending_work) return 1;
    if (!gfx_backend_vulkan_available()) { nvn_gpu_pending_work = 0; return 1; }
    if (!gfx_backend_queue_idle()) {
        char d[96];
        snprintf(d, sizeof d, "%s: GPU work did not reach idle", what);
        nvn_fail(what, d);
        return 0;
    }
    nvn_gpu_pending_work = 0;
    return 1;
}
static void nvnQueueSubmitCommands(queue_t *q, int n, const uint64_t *cmds) { nvn_api("nvnQueueSubmitCommands");
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueSubmitCommands")) return;
    if (!cmds && n) { nvn_fail("nvnQueueSubmitCommands", "null command handle array"); return; }
    q->submits += (uint64_t)n;
    unsigned total = __atomic_add_fetch(&nvn_gpu_submits, (unsigned)n, __ATOMIC_RELAXED);
    if (total <= 8 || getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] GPU submit #%u queue=%p commands=%d (CPU-only host)\n",
                total, (void *)q, n);
    nvn_gpu_pending_work = 1;
    nvn_process_submit_commands(cmds, n);
    if (!nvn_gpu_drain("nvnQueueSubmitCommands"))
        fprintf(stderr, "[totk-nvn] ERROR submit returned before GPU completion\n");
}
static void nvnQueueFlush(queue_t *q) { (void)q; }
static void nvnQueueFinish(queue_t *q) { (void)q; (void)nvn_gpu_drain("nvnQueueFinish"); }
static void nvnQueuePresentTexture(queue_t *q, win_t *w, int index) { nvn_api("nvnQueuePresentTexture");
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueuePresentTexture")) return;
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnQueuePresentTexture(window)")) return;
    (void)index;
    w->presents++;
    unsigned total = __atomic_add_fetch(&nvn_gpu_presents, 1, __ATOMIC_RELAXED);
    if (total <= 4)
        fprintf(stderr, "[totk-nvn] present #%u window=%p index=%d (host backend)\n",
                total, (void *)w, index);
    nvn_gpu_pending_work = 1;
    if (w->native && !runtime_native_window_present(w->native))
        nvn_fail("nvnQueuePresentTexture", "native window present failed");
}
static int nvnQueueGetError(queue_t *q, void *info) {
    (void)q; if (info) memset(info, 0, 0x40); return 0;
}
static size_t nvnQueueGetTotalCommandMemoryUsed(queue_t *q) { return q ? (size_t)q->cmd_used : 0; }
static size_t nvnQueueGetTotalControlMemoryUsed(queue_t *q) { return q ? (size_t)q->ctl_used : 0; }
static size_t nvnQueueGetTotalComputeMemoryUsed(queue_t *q) { return q ? (size_t)q->comp_used : 0; }
static void nvnQueueResetMemoryUsageCounts(queue_t *q) {
    if (q) q->cmd_used = q->comp_used = q->ctl_used = 0;
}
static int nvnQueueFenceSync(queue_t *q, sync_t *s, int condition, int flags);
static int nvnSyncWait(const sync_t *s, uint64_t timeout);
static int nvnQueueWaitSync(queue_t *q, const sync_t *s) { nvn_api("nvnQueueWaitSync");
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueWaitSync")) return 0;
    if (!s || s->magic != NVN_MAGIC('S', 'Y', 'N', 'C')) {
        nvn_fail("nvnQueueWaitSync", "invalid sync");
        return 0;
    }
    int r = nvnSyncWait(s, ~0ull);
    return r == NVN_SYNC_WAIT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
}
static int nvnQueueAcquireTexture(queue_t *q, win_t *w, int *index) {
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueAcquireTexture")) return 1;
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnQueueAcquireTexture(window)")) return 1;
    if (!index) { nvn_fail("nvnQueueAcquireTexture", "null index"); return 1; }
    /* The texture is renderable only after the queued GPU work is done. */
    if (!nvn_gpu_drain("nvnQueueAcquireTexture")) return 1;
    *index = w->ntex ? (w->next++ % w->ntex) : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window                                                              */

static void nvnWindowBuilderSetDefaults(win_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}
static void nvnWindowBuilderSetDevice(win_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnWindowBuilderSetNativeWindow(win_builder_t *b, NvnNativeWindow w) { if (b) b->native = w; }
static void nvnWindowBuilderSetTextures(win_builder_t *b, int n, void *const *t) {
    if (!b) return;
    if (n < 0 || n > 4) { nvn_fail("nvnWindowBuilderSetTextures", "texture count out of range"); n = n < 0 ? 0 : 4; }
    b->ntex = n;
    for (int i = 0; i < n; i++) b->tex[i] = t ? t[i] : NULL;
}
static void nvnWindowBuilderSetPresentInterval(win_builder_t *b, int v) { if (b) b->interval = v; }
static void nvnWindowBuilderSetNumActiveTextures(win_builder_t *b, int n) {
    if (!b) return;
    if (n < 1 || n > 4) { nvn_fail("nvnWindowBuilderSetNumActiveTextures", "out of range"); return; }
    b->active = n;
}
static void *nvnWindowBuilderGetNativeWindow(const win_builder_t *b) { return b ? b->native : NULL; }
static int nvnWindowBuilderGetPresentInterval(const win_builder_t *b) { return b ? b->interval : 0; }
static int nvnWindowBuilderGetNumTextures(const win_builder_t *b) { return b ? b->ntex : 0; }
static void *nvnWindowBuilderGetTexture(const win_builder_t *b, int i) {
    return b && i >= 0 && i < b->ntex ? b->tex[i] : NULL;
}
static void *nvnWindowBuilderGetDevice(const win_builder_t *b) { return b ? b->dev : NULL; }
static int nvnWindowBuilderGetNumActiveTextures(const win_builder_t *b) {
    return b ? (b->active ? b->active : b->ntex) : 0;
}

static uint8_t nvn_default_native[0x40];

/* Window registry used by command-buffer translation: NVN binds render
 * targets, not windows, so the host resolves the window from its color
 * texture. */
static win_t *g_wins[64];
static unsigned g_nwins;

static void nvn_window_register(win_t *w) {
    for (unsigned i = 0; i < g_nwins; i++) if (g_wins[i] == w) return;
    if (g_nwins < sizeof g_wins / sizeof g_wins[0]) g_wins[g_nwins++] = w;
}
static void nvn_window_unregister(win_t *w) {
    for (unsigned i = 0; i < g_nwins; i++) {
        if (g_wins[i] != w) continue;
        g_wins[i] = g_wins[--g_nwins];
        return;
    }
}
static win_t *nvn_window_for_texture(const void *texture) {
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
static void nvn_window_guest_desc(const win_builder_t *b, uint32_t *width,
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

static uint32_t nvnWindowInitialize(win_t *w, const win_builder_t *b) { nvn_api("nvnWindowInitialize");
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
static void nvnWindowFinalize(win_t *w) {
    if (!w) return;
    nvn_window_unregister(w);
    if (w->owned_native && w->native) runtime_native_window_destroy(w->native);
    w->native = NULL;
    w->owned_native = 0;
    w->magic = 0;
}
static void nvnWindowSetDebugLabel(win_t *w, const char *l) { (void)w; (void)l; }
static int nvnWindowAcquireTexture(win_t *w, sync_t *s, int *index) { nvn_api("nvnWindowAcquireTexture");
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
static void *nvnWindowGetNativeWindow(const win_t *w) { nvn_api("nvnWindowGetNativeWindow");
    if (!w || w->magic != NVN_MAGIC('W', 'I', 'N', 'D')) return nvn_default_native;
    return w->native ? w->native : nvn_default_native;
}
static int nvnWindowGetPresentInterval(const win_t *w) { return w ? w->interval : 0; }
static void nvnWindowSetPresentInterval(win_t *w, int v) { if (w) w->interval = v; }
static void nvnWindowSetCrop(win_t *w, int x, int y, int ww, int hh) {
    if (!w) return;
    w->crop[0] = x; w->crop[1] = y; w->crop[2] = ww; w->crop[3] = hh;
}
static void nvnWindowGetCrop(const win_t *w, int *out) {
    if (!out) return;
    if (!w) { memset(out, 0, 16); return; }
    memcpy(out, w->crop, 16);
}
static void nvnWindowSetNumActiveTextures(win_t *w, int n) {
    if (!w) return;
    if (n < 1 || n > w->ntex) { nvn_fail("nvnWindowSetNumActiveTextures", "out of range"); return; }
    w->active = n;
}
static int nvnWindowGetNumActiveTextures(const win_t *w) { return w ? w->active : 0; }
static int nvnWindowGetNumTextures(const win_t *w) { return w ? w->ntex : 0; }

/* Host-side round-trip check used by probe-vi: the native-window pointer the
 * VI layer hands out must survive builder -> window -> getter unchanged. */
extern int runtime_native_window_clear(void *p, uint32_t rgba);
int runtime_nvn_submit_selftest(void) {
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0x77);
    if (!nw) return 0;
    dev_builder_t db;
    nvn_dev_t d;
    queue_builder_t qb;
    queue_t q;
    win_builder_t wb;
    win_t w;
    memset(&db, 0, sizeof db);
    memset(&d, 0, sizeof d);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    memset(&qb, 0, sizeof qb);
    qb.dev = &d;
    qb.flags = 1;
    qb.cmd_sz = 0x10000;
    qb.ctl_sz = 0x1000;
    qb.mem_sz = 0x2000;
    memset(&q, 0, sizeof q);
    if (!nvnQueueInitialize(&q, &qb)) return 0;
    memset(&wb, 0, sizeof wb);
    wb.dev = &d;
    wb.native = nw;
    wb.ntex = 1;
    wb.tex[0] = (void *)(uintptr_t)1;
    memset(&w, 0, sizeof w);
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    uint64_t handle = 0x100000001ull;
    nvnQueueSubmitCommands(&q, 1, &handle);
    if (!runtime_native_window_clear(nw, 0xff102030u)) return 0;
    nvnQueuePresentTexture(&q, &w, 0);
    if (q.submits != 1 || w.presents != 1) return 0;
    nvnWindowFinalize(&w);
    return 1;
}

int runtime_nvn_window_selftest(void *nw) {
    win_builder_t b;
    win_t w;
    memset(&b, 0, sizeof b);
    memset(&w, 0, sizeof w);
    b.native = nw;
    b.ntex = 1;
    b.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &b)) return 0;
    if (nvnWindowGetNativeWindow(&w) != nw) return 0;
    if (nvnWindowGetNumTextures(&w) != 1) return 0;
    nvnWindowFinalize(&w);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Memory pool                                                         */

static void nvnMemoryPoolBuilderSetDefaults(pool_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
}
static void nvnMemoryPoolBuilderSetDevice(pool_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnMemoryPoolBuilderSetStorage(pool_builder_t *b, void *m, size_t n) {
    if (b) { b->mem = m; b->size = n; }
}
static void nvnMemoryPoolBuilderSetFlags(pool_builder_t *b, int f) { if (b) b->flags = (uint32_t)f; }
static void *nvnMemoryPoolBuilderGetMemory(const pool_builder_t *b) { return b ? b->mem : NULL; }
static size_t nvnMemoryPoolBuilderGetSize(const pool_builder_t *b) { return b ? (size_t)b->size : 0; }
static int nvnMemoryPoolBuilderGetFlags(const pool_builder_t *b) { return b ? (int)b->flags : 0; }
static void *nvnMemoryPoolBuilderGetDevice(const pool_builder_t *b) { return b ? b->dev : NULL; }

static uint32_t nvnMemoryPoolInitialize(pool_t *p, const pool_builder_t *b) { nvn_api("nvnMemoryPoolInitialize");
    if (!p || !b) { nvn_fail("nvnMemoryPoolInitialize", "null pool/builder"); return 0; }
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('P', 'O', 'O', 'L');
    p->flags = b->flags;
    p->size = b->size;
    p->dev = b->dev;
    p->mem = b->mem;
    if (!p->mem && p->size) {
        /* Physical/virtual pools do not have guest storage in this host.
         * Give them a private CPU mapping so map/address calls have real
         * pointer semantics; no GPU sees this memory. */
        void *m = mmap(NULL, (size_t)p->size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) {
            char d[64]; snprintf(d, sizeof d, "size %#llx", (unsigned long long)p->size);
            nvn_fail("nvnMemoryPoolInitialize", d);
            p->magic = 0;
            return 0;
        }
        p->mem = m;
        p->owned = 1;
        p->host_size = p->size;
    }
    if (!p->mem && !(p->flags & NVN_POOL_GPU_NO_ACCESS)) {
        nvn_fail("nvnMemoryPoolInitialize", "pool has no storage");
        p->magic = 0;
        return 0;
    }
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] MemoryPoolInitialize %p mem=%p size=%#llx flags=%#x%s\n",
                (void *)p, (void *)p->mem, (unsigned long long)p->size, p->flags,
                p->owned ? " (host fallback)" : "");
    return 1;
}
static void nvnMemoryPoolFinalize(pool_t *p) {
    if (!p) return;
    if (p->owned && p->mem) munmap(p->mem, (size_t)p->host_size);
    p->magic = 0;
}
static void nvnMemoryPoolSetDebugLabel(pool_t *p, const char *l) { (void)p; (void)l; }
static void *nvnMemoryPoolMap(const pool_t *p) { nvn_api("nvnMemoryPoolMap");
    if (!p || p->magic != NVN_MAGIC('P', 'O', 'O', 'L')) {
        nvn_fail("nvnMemoryPoolMap", "invalid pool object");
        return NULL;
    }
    /* A CPU reader must observe every GPU write submitted before this call. */
    if (!nvn_gpu_drain("nvnMemoryPoolMap")) return NULL;
    if (p->flags & NVN_POOL_CPU_NO_ACCESS) {
        /* NVN marks this pool CPU_NO_ACCESS for the guest, but the HLE keeps
         * all GPU resources in host memory and needs the mapping for shader,
         * vertex and index translation.  Return it; the guest cannot tell. */
        if (nvn_trace())
            fprintf(stderr, "[totk-nvn] MemoryPoolMap CPU_NO_ACCESS pool=%p -> host mapping %p\n",
                    (const void *)p, (void *)p->mem);
        return p->mem;
    }
    return p->mem;
}
static void nvnMemoryPoolFlushMappedRange(const pool_t *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
static void nvnMemoryPoolInvalidateMappedRange(const pool_t *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
static uint64_t nvnMemoryPoolGetBufferAddress(const pool_t *p) { nvn_api("nvnMemoryPoolGetBufferAddress");
    return p && p->magic == NVN_MAGIC('P', 'O', 'O', 'L') ? (uint64_t)(uintptr_t)p->mem : 0;
}
static uint32_t nvnMemoryPoolMapVirtual(pool_t *p, int n, const void *reqs) {
    (void)reqs;
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnMemoryPoolMapVirtual")) return 0;
    if (n <= 0) { nvn_fail("nvnMemoryPoolMapVirtual", "empty mapping request"); return 0; }
    p->map_count += (uint32_t)n;
    return 1;
}
static size_t nvnMemoryPoolGetSize(const pool_t *p) { return p ? (size_t)p->size : 0; }
static int nvnMemoryPoolGetFlags(const pool_t *p) { return p ? (int)p->flags : 0; }

/* ------------------------------------------------------------------ */
/* Buffer                                                              */

static void nvnBufferBuilderSetDefaults(buf_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
}
static void nvnBufferBuilderSetDevice(buf_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnBufferBuilderSetStorage(buf_builder_t *b, void *p, ptrdiff_t off, size_t size) {
    if (b) { b->pool = p; b->off = off; b->size = size; }
}
static void *nvnBufferBuilderGetDevice(const buf_builder_t *b) { return b ? b->dev : NULL; }
static void *nvnBufferBuilderGetMemoryPool(const buf_builder_t *b) { return b ? b->pool : NULL; }
static ptrdiff_t nvnBufferBuilderGetMemoryOffset(const buf_builder_t *b) { return b ? (ptrdiff_t)b->off : 0; }
static size_t nvnBufferBuilderGetSize(const buf_builder_t *b) { return b ? (size_t)b->size : 0; }

static uint32_t nvnBufferInitialize(buf_t *b, const buf_builder_t *s) { nvn_api("nvnBufferInitialize");
    if (!b || !s) { nvn_fail("nvnBufferInitialize", "null buffer/builder"); return 0; }
    pool_t *p = s->pool;
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnBufferInitialize(pool)")) return 0;
    if (s->off < 0 || (uint64_t)s->off > p->size || s->size > p->size - (uint64_t)s->off) {
        if (nvn_fail_count++ < 48)
            fprintf(stderr, "[totk-nvn] nvnBufferInitialize range off=%lld size=%llu pool_size=%llu\n",
                    (long long)s->off, (unsigned long long)s->size, (unsigned long long)p->size);
        char d[96]; snprintf(d, sizeof d, "range off=%lld size=%llu pool=%llu",
                             (long long)s->off, (unsigned long long)s->size,
                             (unsigned long long)p->size);
        nvn_fail("nvnBufferInitialize", d);
        return 0;
    }
    memset(b, 0, sizeof *b);
    b->magic = NVN_MAGIC('B', 'U', 'F', 0);
    b->pool = p;
    b->off = s->off;
    b->size = s->size;
    return 1;
}
static void nvnBufferFinalize(buf_t *b) { if (b) b->magic = 0; }
static void nvnBufferSetDebugLabel(buf_t *b, const char *l) { (void)b; (void)l; }
static void *nvnBufferMap(const buf_t *b) { nvn_api("nvnBufferMap");
    if (!b || b->magic != NVN_MAGIC('B', 'U', 'F', 0) || !b->pool || !b->pool->mem) {
        if (nvn_fail_count++ < 48)
            fprintf(stderr, "[totk-nvn] nvnBufferMap invalid buffer=%p magic=%#x pool=%p mem=%p off=%lld size=%llu\n",
                    (const void *)b, b ? b->magic : 0, b ? (void *)b->pool : NULL,
                    (b && b->pool) ? (void *)b->pool->mem : NULL,
                    (long long)(b ? b->off : 0), (unsigned long long)(b ? b->size : 0));
        return NULL;
    }
    if (!nvn_gpu_drain("nvnBufferMap")) return NULL;
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] BufferMap %p pool=%p mem=%p off=%lld size=%llu -> %p thread=%#x\n",
                (const void *)b, (void *)b->pool, (void *)b->pool->mem,
                (long long)b->off, (unsigned long long)b->size,
                (void *)(b->pool->mem + b->off), runtime_thread_current_handle());
    return b->pool->mem + b->off;
}
static uint64_t nvnBufferGetAddress(const buf_t *b) {
    return (uint64_t)(uintptr_t)nvnBufferMap(b);
}
static void nvnBufferFlushMappedRange(const buf_t *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
static void nvnBufferInvalidateMappedRange(const buf_t *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
static void *nvnBufferGetMemoryPool(const buf_t *b) { return b ? b->pool : NULL; }
static ptrdiff_t nvnBufferGetMemoryOffset(const buf_t *b) { return b ? (ptrdiff_t)b->off : 0; }
static size_t nvnBufferGetSize(const buf_t *b) { return b ? (size_t)b->size : 0; }
static uint64_t nvnBufferGetDebugID(const buf_t *b) { return b ? (uint64_t)(uintptr_t)b : 0; }

/* ------------------------------------------------------------------ */
/* Texture                                                             */

static void nvnTextureBuilderSetDefaults(tex_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.target = NVN_TARGET_2D;
    b->d.format = 0x25; /* NVN_FORMAT_RGBA8 */
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}
static void nvnTextureBuilderSetDevice(tex_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnTextureBuilderSetFlags(tex_builder_t *b, int f) { if (b) b->d.flags = f; }
static void nvnTextureBuilderSetTarget(tex_builder_t *b, int t) { if (b) b->d.target = t; }
static void nvnTextureBuilderSetWidth(tex_builder_t *b, int v) { if (b) b->d.width = (uint32_t)v; }
static void nvnTextureBuilderSetHeight(tex_builder_t *b, int v) { if (b) b->d.height = (uint32_t)v; }
static void nvnTextureBuilderSetDepth(tex_builder_t *b, int v) { if (b) b->d.depth = (uint32_t)v; }
static void nvnTextureBuilderSetSize1D(tex_builder_t *b, int w) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = b->d.depth = 1; }
}
static void nvnTextureBuilderSetSize2D(tex_builder_t *b, int w, int h) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = (uint32_t)h; b->d.depth = 1; }
}
static void nvnTextureBuilderSetSize3D(tex_builder_t *b, int w, int h, int d) {
    if (b) { b->d.width = (uint32_t)w; b->d.height = (uint32_t)h; b->d.depth = (uint32_t)d; }
}
static void nvnTextureBuilderSetLevels(tex_builder_t *b, int v) { if (b) b->d.levels = (uint32_t)v; }
static void nvnTextureBuilderSetFormat(tex_builder_t *b, int f) { if (b) b->d.format = f; }
static void nvnTextureBuilderSetSamples(tex_builder_t *b, int s) { if (b) b->d.samples = s; }
static void nvnTextureBuilderSetSwizzle(tex_builder_t *b, int r, int g, int bl, int a) {
    (void)b; (void)r; (void)g; (void)bl; (void)a;
}
static void nvnTextureBuilderSetDepthStencilMode(tex_builder_t *b, int m) { (void)b; (void)m; }
static void nvnTextureBuilderSetStorage(tex_builder_t *b, void *p, ptrdiff_t off) {
    if (b) { b->pool = p; b->off = off; }
}
static void nvnTextureBuilderSetPackagedTextureData(tex_builder_t *b, const void *data) { if (b) b->pkg_data = data; }
static void nvnTextureBuilderSetPackagedTextureLayout(tex_builder_t *b, const void *layout) {
    if (!b) return;
    b->has_layout = layout != NULL;
    if (layout) memcpy(b->layout, layout, 8);
    b->d.pkg_block_h = layout ? (int)((const uint8_t *)layout)[0] + 1 : 0;
}
static void nvnTextureBuilderSetStride(tex_builder_t *b, ptrdiff_t s) { if (b) b->d.stride = (uint32_t)s; }
static void nvnTextureBuilderSetGLTextureName(tex_builder_t *b, uint32_t n) { (void)b; (void)n; }
static size_t nvnTextureBuilderGetStorageSize(const tex_builder_t *b) { nvn_api("nvnTextureBuilderGetStorageSize");
    if (!b) return 0;
    tex_layout L; nvn_tex_layout(&b->d, &L);
    return (size_t)L.size;
}
static size_t nvnTextureBuilderGetStorageAlignment(const tex_builder_t *b) {
    if (!b) return 512;
    tex_layout L; nvn_tex_layout(&b->d, &L);
    return L.align;
}
static ptrdiff_t nvnTextureBuilderGetViewOffset(const tex_builder_t *b, const uint8_t *v) {
    if (!b || !v) return 0;
    tex_layout L; nvn_tex_layout(&b->d, &L);
    int set_levels = v[0] & 1, set_layers = (v[0] >> 1) & 1;
    unsigned base_level = v[1], min_layer = (unsigned)v[4] | ((unsigned)v[5] << 8);
    uint64_t off = set_levels ? nvn_tex_level_offset(&b->d, base_level) : 0;
    if (set_layers) off += (uint64_t)min_layer * L.layer_size;
    return (ptrdiff_t)off;
}
static const void *nvnTextureBuilderGetPackagedTextureLayout(const tex_builder_t *b) {
    return b && b->has_layout ? b->layout : NULL;
}
static int nvnTextureBuilderGetRawStorageClass(const tex_builder_t *b) { (void)b; return 1; }
static int nvnTextureBuilderGetStorageClass(const tex_builder_t *b) {
    return b && b->d.target == NVN_TARGET_BUFFER ? 0 : 1;
}
static int nvnTextureBuilderGetFlags(const tex_builder_t *b) { return b ? b->d.flags : 0; }
static int nvnTextureBuilderGetTarget(const tex_builder_t *b) { return b ? b->d.target : 0; }
static int nvnTextureBuilderGetWidth(const tex_builder_t *b) { return b ? (int)b->d.width : 0; }
static int nvnTextureBuilderGetHeight(const tex_builder_t *b) { return b ? (int)b->d.height : 0; }
static int nvnTextureBuilderGetDepth(const tex_builder_t *b) { return b ? (int)b->d.depth : 0; }
static int nvnTextureBuilderGetLevels(const tex_builder_t *b) { return b ? (int)b->d.levels : 0; }
static int nvnTextureBuilderGetFormat(const tex_builder_t *b) { return b ? b->d.format : 0; }
static int nvnTextureBuilderGetSamples(const tex_builder_t *b) { return b ? b->d.samples : 0; }
static void nvnTextureBuilderGetSwizzle(const tex_builder_t *b, int *r, int *g, int *bl, int *a) {
    (void)b; if (r) *r = 2; if (g) *g = 3; if (bl) *bl = 4; if (a) *a = 5;
}
static int nvnTextureBuilderGetDepthStencilMode(const tex_builder_t *b) { (void)b; return 0; }
static const void *nvnTextureBuilderGetPackagedTextureData(const tex_builder_t *b) { return b ? b->pkg_data : NULL; }
static ptrdiff_t nvnTextureBuilderGetStride(const tex_builder_t *b) { return b ? (ptrdiff_t)b->d.stride : 0; }
static void nvnTextureBuilderGetSparseTileLayout(const tex_builder_t *b, void *out) { (void)b; if (out) memset(out, 0, 0x28); }
static uint32_t nvnTextureBuilderGetGLTextureName(const tex_builder_t *b) { (void)b; return 0; }
static size_t nvnTextureBuilderGetZCullStorageSize(const tex_builder_t *b) { (void)b; return 0; }
static void *nvnTextureBuilderGetMemoryPool(const tex_builder_t *b) { return b ? b->pool : NULL; }
static ptrdiff_t nvnTextureBuilderGetMemoryOffset(const tex_builder_t *b) { return b ? (ptrdiff_t)b->off : 0; }
static void *nvnTextureBuilderGetDevice(const tex_builder_t *b) { return b ? b->dev : NULL; }

static uint32_t nvnTextureInitialize(tex_t *t, const tex_builder_t *b) { nvn_api("nvnTextureInitialize");
    if (!t || !b) { nvn_fail("nvnTextureInitialize", "null texture/builder"); return 0; }
    memset(t, 0, sizeof *t);
    t->magic = NVN_MAGIC('T', 'E', 'X', 0);
    t->d = b->d;
    t->has_layout = b->has_layout;
    memcpy(t->layout, b->layout, 8);
    t->pool = b->pool;
    t->off = b->off;
    nvn_tex_layout(&t->d, &t->L);
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
static int nvn_texture_store_bridge(tex_t *t, GfxTexture *g) {
    for (unsigned i = 0; i < g_tex_bridge_count; i++) {
        if (g_tex_bridges[i].tex == t) {
            if (g_tex_bridges[i].gfx) gfx_backend_texture_destroy(g_tex_bridges[i].gfx);
            g_tex_bridges[i].gfx = g;
            return 1;
        }
    }
    if (g_tex_bridge_count < sizeof g_tex_bridges / sizeof g_tex_bridges[0]) {
        g_tex_bridges[g_tex_bridge_count++] = (tex_bridge_t){ t, g };
        return 1;
    }
    gfx_backend_texture_destroy(g);
    return 0;
}
/* The guest is the source of truth: a CPU write/flush invalidates the host
 * copy so the next bind re-uploads the updated storage. */
static void nvn_texture_bridge_drop(const tex_t *t) {
    for (unsigned i = 0; i < g_tex_bridge_count; i++) {
        if (g_tex_bridges[i].tex != t) continue;
        if (g_tex_bridges[i].gfx) gfx_backend_texture_destroy(g_tex_bridges[i].gfx);
        g_tex_bridges[i] = g_tex_bridges[--g_tex_bridge_count];
        return;
    }
}


int runtime_nvn_texture_upload_rgba8(void *texture, const uint8_t *pixels,
                                    uint32_t width, uint32_t height) {
    tex_t *t = texture;
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0) || !pixels) return 0;
    GfxTexture *g = gfx_backend_texture_create_rgba8(width, height, pixels);
    if (!g) return 0;
    return nvn_texture_store_bridge(t, g);
}

/* Guest storage -> linear -> Vulkan for every mip/layer of the shapes the
 * first frame uses.  Guest storage stays the source of truth: the upload is
 * lazy (first bind) and unsupported target/format combinations are explicit
 * failures, never a silent RGBA8 substitute. */
extern int runtime_guest_texture_deswizzle(const void *storage, void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h);
extern int runtime_guest_texture_swizzle(void *storage, const void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h);
extern int runtime_guest_texture_deswizzle_at(const void *storage, void *linear, size_t pitch,
    int target, int format, int flags, int samples, uint32_t width, uint32_t height,
    uint32_t depth, uint32_t levels, uint32_t stride, int pkg_block_h,
    unsigned level, unsigned layer, unsigned z, unsigned z_count);
extern int runtime_guest_texture_level_geom(int target, int format, int flags, int samples,
    uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t stride,
    int pkg_block_h, unsigned level, uint32_t *row_bytes, uint32_t *rows,
    uint32_t *slices, uint32_t *bpb);

static int nvn_texture_gfx_target(int target) {
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

static uint32_t nvn_texture_gfx_layers(const tex_desc *d) {
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
        static unsigned logged;
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
        total += is_3d ? slices : layers;
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
        uint32_t zcount = is_3d ? slices : 1;
        uint32_t lcount = is_3d ? 1 : layers;
        for (uint32_t layer = 0; layer < lcount && ok; layer++) {
            for (uint32_t z = 0; z < zcount && ok; z++) {
                uint8_t *buf = malloc(slice_bytes ? slice_bytes : 1);
                if (!buf) { ok = 0; break; }
                if (runtime_guest_texture_deswizzle_at(t->mem, buf, rb, t->d.target,
                                                       t->d.format, t->d.flags, t->d.samples,
                                                       w, h, depth, levels, t->d.stride,
                                                       t->d.pkg_block_h, l, layer,
                                                       is_3d ? z : 0, 1)) {
                    fprintf(stderr, "[totk-nvn] ERROR deswizzle level=%u layer=%u z=%u"
                                    " (target=%d format=%#x)\n",
                            l, layer, z, t->d.target, t->d.format);
                    free(buf);
                    ok = 0;
                    break;
                }
                blocks[nsub] = buf;
                subs[nsub].level = l;
                subs[nsub].layer = is_3d ? z : layer;
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

static void *nvn_texture_gfx(const tex_t *t) {
    for (unsigned i = 0; i < g_tex_bridge_count; i++)
        if (g_tex_bridges[i].tex == t) return g_tex_bridges[i].gfx;
    return NULL;
}

/* First bind uploads the guest block-linear texture.  The macOS runtime does
 * the equivalent on texture creation; the Android path keeps the guest storage
 * as the source of truth and uploads lazily. */
static GfxTexture *nvn_texture_gfx_ready(const tex_t *t) {
    GfxTexture *g = nvn_texture_gfx(t);
    if (g) return g;
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return NULL;
    if (!runtime_nvn_texture_upload_guest((void *)t)) return NULL;
    return nvn_texture_gfx(t);
}

static GfxSampler *nvn_sampler_gfx(const samp_t *s) {
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

static void nvnTextureFinalize(tex_t *t) {
    if (!t) return;
    nvn_tex_registry_del(t);
    for (unsigned i = 0; i < g_tex_bridge_count; i++) {
        if (g_tex_bridges[i].tex == t) {
            if (g_tex_bridges[i].gfx) gfx_backend_texture_destroy(g_tex_bridges[i].gfx);
            g_tex_bridges[i] = g_tex_bridges[--g_tex_bridge_count];
            break;
        }
    }
    t->magic = 0;
}
static void nvnTextureSetDebugLabel(tex_t *t, const char *l) { (void)t; (void)l; }
static int nvnTextureGetStorageClass(const tex_t *t) { return t && t->d.target == NVN_TARGET_BUFFER ? 0 : 1; }
static int nvnTextureGetFlags(const tex_t *t) { return t ? t->d.flags : 0; }
static int nvnTextureGetTarget(const tex_t *t) { return t ? t->d.target : 0; }
static int nvnTextureGetWidth(const tex_t *t) { return t ? (int)t->d.width : 0; }
static int nvnTextureGetHeight(const tex_t *t) { return t ? (int)t->d.height : 0; }
static int nvnTextureGetDepth(const tex_t *t) { return t ? (int)t->d.depth : 0; }
static int nvnTextureGetLevels(const tex_t *t) { return t ? (int)t->d.levels : 0; }
static int nvnTextureGetFormat(const tex_t *t) { return t ? t->d.format : 0; }
static int nvnTextureGetSamples(const tex_t *t) { return t ? t->d.samples : 0; }
static int nvnTextureGetStride(const tex_t *t) { return t ? (int)t->d.stride : 0; }
static void nvnTextureGetSwizzle(const tex_t *t, int *r, int *g, int *bl, int *a) {
    (void)t; if (r) *r = 2; if (g) *g = 3; if (bl) *bl = 4; if (a) *a = 5;
}
static int nvnTextureGetDepthStencilMode(const tex_t *t) { (void)t; return 0; }
static int nvnTextureGetStorageSize(const tex_t *t) { return t ? (int)t->L.size : 0; }
static int64_t nvnTextureGetViewOffset(const tex_t *t, const void *view) { nvn_api("nvnTextureGetViewOffset");
    if (!t || !view) return 0;
    const uint8_t *v = view;
    int set_levels = v[0] & 1, set_layers = (v[0] >> 1) & 1;
    unsigned base_level = v[1], min_layer = (unsigned)v[4] | ((unsigned)v[5] << 8);
    uint64_t off = set_levels ? nvn_tex_level_offset(&t->d, base_level) : 0;
    if (set_layers) off += (uint64_t)min_layer * t->L.layer_size;
    return (int64_t)off;
}
static uint64_t nvnTextureGetTextureAddress(const tex_t *t) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return 0;
    return t->mem ? (uint64_t)(uintptr_t)t->mem : 0;
}
static void nvnTextureGetSparseTileLayout(const tex_t *t, void *out) { (void)t; if (out) memset(out, 0, 0x28); }
static void nvnTextureFlushTexels(const tex_t *t, const void *v, const void *r) { (void)t; (void)v; (void)r; }
static void nvnTextureInvalidateTexels(const tex_t *t, const void *v, const void *r) { (void)t; (void)v; (void)r; }
static void *nvnTextureGetMemoryPool(const tex_t *t) { return t ? t->pool : NULL; }
static int64_t nvnTextureGetMemoryOffset(const tex_t *t) { return t ? t->off : 0; }
static int nvnTextureGetRawStorageClass(const tex_t *t) { (void)t; return 1; }
static size_t nvnTextureGetZCullStorageSize(const tex_t *t) { (void)t; return 0; }
static uint64_t nvnTextureGetDebugID(const tex_t *t) { return t ? (uint64_t)(uintptr_t)t : 0; }
static uint32_t nvnTextureCompare(const tex_t *a, const tex_t *b) {
    if (!a || !b) return 0;
    return !memcmp(&a->d, &b->d, sizeof a->d) && a->mem == b->mem;
}

/* A CPU-side texel copy is enough for this host: nothing consumes the GPU
 * layout, and ReadTexels round-trips the same bytes. */
static void nvn_tex_copy(const tex_t *t, const void *view, const int *region,
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
    } else {
        if ((size_t)(img * d) > avail) d = (int)(avail / (size_t)(img ? img : 1));
        for (int zz = 0; zz < d; zz++)
            for (int yy = 0; yy < h; yy++)
                memcpy((uint8_t *)dst + (size_t)zz * img + (size_t)yy * row,
                       t->mem + off + (size_t)zz * img + (size_t)yy * row,
                       rb < (size_t)row ? rb : (size_t)row);
    }
}
static void nvnTextureWriteTexels(const tex_t *t, const void *v, const int *r, const void *src) { nvn_api("nvnTextureWriteTexels");
    nvn_tex_copy(t, v, r, NULL, src, 0, 0, 0);
    nvn_texture_bridge_drop(t);
}
static void nvnTextureWriteTexelsStrided(const tex_t *t, const void *v, const int *r,
                                         const void *src, ptrdiff_t row, ptrdiff_t img) { nvn_api("nvnTextureWriteTexelsStrided");
    nvn_tex_copy(t, v, r, NULL, src, row, img, 0);
    nvn_texture_bridge_drop(t);
}
static void nvnTextureReadTexels(const tex_t *t, const void *v, const int *r, void *dst) { nvn_api("nvnTextureReadTexels");
    nvn_tex_copy(t, v, r, dst, NULL, 0, 0, 1);
}
static void nvnTextureReadTexelsStrided(const tex_t *t, const void *v, const int *r,
                                        void *dst, ptrdiff_t row, ptrdiff_t img) { nvn_api("nvnTextureReadTexelsStrided");
    nvn_tex_copy(t, v, r, dst, NULL, row, img, 1);
}

/* Texture view: 16 bytes, the layout TOTK uses. */
static void nvnTextureViewSetDefaults(uint8_t *v) { if (v) memset(v, 0, NVN_TEXTURE_VIEW_SIZE); }
/* NVNtextureView (16 bytes): flags byte 0, base_level byte 1, nlevels byte 2,
 * target byte 3, min_layer u16 at 4, nlayers u16 at 6, format u16 at 8,
 * swizzle bytes 10..13.  This matches the guest's 16-byte view layout. */
static void nvnTextureViewSetLevels(uint8_t *v, int base, int n) {
    if (!v || base < 0 || base > 255 || n < 0 || n > 255) { nvn_fail("nvnTextureViewSetLevels", "out of range"); return; }
    v[0] |= 1; v[1] = (uint8_t)base; v[2] = (uint8_t)n;
}
static void nvnTextureViewSetLayers(uint8_t *v, int min, int n) {
    if (!v || min < 0 || min > 65535 || n < 0 || n > 65535) { nvn_fail("nvnTextureViewSetLayers", "out of range"); return; }
    v[0] |= 2; v[4] = (uint8_t)min; v[5] = (uint8_t)(min >> 8);
    v[6] = (uint8_t)n; v[7] = (uint8_t)(n >> 8);
}
static void nvnTextureViewSetFormat(uint8_t *v, int f) {
    if (!v) return; v[0] |= 4; v[8] = (uint8_t)f; v[9] = (uint8_t)(f >> 8);
}
static void nvnTextureViewSetSwizzle(uint8_t *v, int r, int g, int bl, int a) {
    if (!v) return; v[0] |= 16; v[10] = (uint8_t)r; v[11] = (uint8_t)g; v[12] = (uint8_t)bl; v[13] = (uint8_t)a;
}
static void nvnTextureViewSetDepthStencilMode(uint8_t *v, int m) { (void)v; (void)m; }
static void nvnTextureViewSetTarget(uint8_t *v, int t) { if (v) { v[0] |= 8; v[3] = (uint8_t)t; } }
static uint32_t nvnTextureViewGetLevels(const uint8_t *v, int *base, int *n) {
    if (!v || !base || !n) return 0;
    *base = v[1]; *n = v[2]; return v[0] & 1;
}
static uint32_t nvnTextureViewGetLayers(const uint8_t *v, int *min, int *n) {
    if (!v || !min || !n) return 0;
    *min = v[4] | (v[5] << 8); *n = v[6] | (v[7] << 8); return (v[0] >> 1) & 1;
}
static uint32_t nvnTextureViewGetFormat(const uint8_t *v, int *f) {
    if (!v || !f) return 0; *f = v[8] | (v[9] << 8); return (v[0] >> 2) & 1;
}
static uint32_t nvnTextureViewGetSwizzle(const uint8_t *v, int *r, int *g, int *bl, int *a) {
    if (!v || !r || !g || !bl || !a) return 0;
    *r = v[10]; *g = v[11]; *bl = v[12]; *a = v[13]; return (v[0] >> 4) & 1;
}
static uint32_t nvnTextureViewGetDepthStencilMode(const uint8_t *v, int *m) {
    if (!v || !m) return 0; *m = 0; return 0;
}
static uint32_t nvnTextureViewGetTarget(const uint8_t *v, int *t) {
    if (!v || !t) return 0; *t = v[3]; return (v[0] >> 3) & 1;
}
static uint32_t nvnTextureViewCompare(const uint8_t *a, const uint8_t *b) {
    return a && b && !memcmp(a, b, NVN_TEXTURE_VIEW_SIZE);
}

/* ------------------------------------------------------------------ */
/* Sampler                                                             */

static void nvnSamplerBuilderSetDefaults(samp_builder_t *b) {
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
static void nvnSamplerBuilderSetDevice(samp_builder_t *b, void *d) { if (b) b->dev = d; }
static void nvnSamplerBuilderSetMinMagFilter(samp_builder_t *b, int min, int mag) {
    if (b) { b->p.minf = (uint8_t)min; b->p.magf = (uint8_t)mag; }
}
static void nvnSamplerBuilderSetWrapMode(samp_builder_t *b, int s, int t, int r) {
    if (b) { b->p.ws = (uint8_t)s; b->p.wt = (uint8_t)t; b->p.wr = (uint8_t)r; }
}
static void nvnSamplerBuilderSetLodClamp(samp_builder_t *b, float lo, float hi) {
    if (b) { b->p.lod_min = lo; b->p.lod_max = hi; }
}
static void nvnSamplerBuilderSetLodBias(samp_builder_t *b, float v) { if (b) b->p.lod_bias = v; }
static void nvnSamplerBuilderSetCompare(samp_builder_t *b, int mode, int func) {
    if (b) { b->p.cmp_mode = (uint8_t)mode; b->p.cmp_func = (uint8_t)func; }
}
static void nvnSamplerBuilderSetBorderColor(samp_builder_t *b, const float *c) {
    if (b && c) memcpy(b->p.border, c, sizeof b->p.border);
}
static void nvnSamplerBuilderSetBorderColori(samp_builder_t *b, const int *c) {
    if (!b || !c) return;
    for (int i = 0; i < 4; i++) b->p.border[i] = (float)c[i];
}
static void nvnSamplerBuilderSetBorderColorui(samp_builder_t *b, const uint32_t *c) {
    if (!b || !c) return;
    for (int i = 0; i < 4; i++) b->p.border[i] = (float)c[i];
}
static void nvnSamplerBuilderSetMaxAnisotropy(samp_builder_t *b, float v) { if (b) b->p.aniso = v; }
static void nvnSamplerBuilderSetReductionFilter(samp_builder_t *b, int v) { if (b) b->p.reduction = (uint8_t)v; }
static void nvnSamplerBuilderSetLodSnap(samp_builder_t *b, float v) { if (b) b->p.lod_snap = v; }
static void *nvnSamplerBuilderGetDevice(const samp_builder_t *b) { return b ? b->dev : NULL; }
static int nvnSamplerBuilderGetMinMagFilter(const samp_builder_t *b, int *min, int *mag) {
    if (!b || !min || !mag) return 0;
    *min = b->p.minf; *mag = b->p.magf; return 1;
}
static int nvnSamplerBuilderGetWrapMode(const samp_builder_t *b, int *s, int *t, int *r) {
    if (!b || !s || !t || !r) return 0;
    *s = b->p.ws; *t = b->p.wt; *r = b->p.wr; return 1;
}
static void nvnSamplerBuilderGetLodClamp(const samp_builder_t *b, float *lo, float *hi) {
    if (b && lo && hi) { *lo = b->p.lod_min; *hi = b->p.lod_max; }
}
static float nvnSamplerBuilderGetLodBias(const samp_builder_t *b) { return b ? b->p.lod_bias : 0; }
static void nvnSamplerBuilderGetCompare(const samp_builder_t *b, int *mode, int *func) {
    if (b && mode && func) { *mode = b->p.cmp_mode; *func = b->p.cmp_func; }
}
static void nvnSamplerBuilderGetBorderColor(const samp_builder_t *b, float *c) { if (b && c) memcpy(c, b->p.border, 16); }
static void nvnSamplerBuilderGetBorderColori(const samp_builder_t *b, int *c) {
    if (b && c) for (int i = 0; i < 4; i++) c[i] = (int)b->p.border[i];
}
static void nvnSamplerBuilderGetBorderColorui(const samp_builder_t *b, uint32_t *c) {
    if (b && c) for (int i = 0; i < 4; i++) c[i] = (uint32_t)b->p.border[i];
}
static float nvnSamplerBuilderGetMaxAnisotropy(const samp_builder_t *b) { return b ? b->p.aniso : 0; }
static int nvnSamplerBuilderGetReductionFilter(const samp_builder_t *b) { return b ? b->p.reduction : 0; }
static float nvnSamplerBuilderGetLodSnap(const samp_builder_t *b) { return b ? b->p.lod_snap : 0; }

static uint32_t nvnSamplerInitialize(samp_t *s, const samp_builder_t *b) { nvn_api("nvnSamplerInitialize");
    if (!s || !b) { nvn_fail("nvnSamplerInitialize", "null sampler/builder"); return 0; }
    memset(s, 0, sizeof *s);
    s->magic = NVN_MAGIC('S', 'A', 'M', 'P');
    s->p = b->p;
    return 1;
}
static void nvnSamplerFinalize(samp_t *s) {
    if (!s) return;
    for (unsigned i = 0; i < g_samp_bridge_count; i++)
        if (g_samp_bridges[i].samp == s) {
            gfx_backend_sampler_destroy(g_samp_bridges[i].gfx);
            g_samp_bridges[i] = g_samp_bridges[--g_samp_bridge_count];
            break;
        }
    s->magic = 0;
}
static void nvnSamplerSetDebugLabel(samp_t *s, const char *l) { (void)s; (void)l; }
static int nvnSamplerGetMinMagFilter(const samp_t *s, int *min, int *mag) {
    if (!s || !min || !mag) return 0; *min = s->p.minf; *mag = s->p.magf; return 1;
}
static int nvnSamplerGetWrapMode(const samp_t *s, int *x, int *y, int *z) {
    if (!s || !x || !y || !z) return 0; *x = s->p.ws; *y = s->p.wt; *z = s->p.wr; return 1;
}
static void nvnSamplerGetLodClamp(const samp_t *s, float *lo, float *hi) {
    if (s && lo && hi) { *lo = s->p.lod_min; *hi = s->p.lod_max; }
}
static float nvnSamplerGetLodBias(const samp_t *s) { return s ? s->p.lod_bias : 0; }
static void nvnSamplerGetCompare(const samp_t *s, int *mode, int *func) {
    if (s && mode && func) { *mode = s->p.cmp_mode; *func = s->p.cmp_func; }
}
static void nvnSamplerGetBorderColor(const samp_t *s, float *c) { if (s && c) memcpy(c, s->p.border, 16); }
static void nvnSamplerGetBorderColori(const samp_t *s, int *c) { if (s && c) for (int i = 0; i < 4; i++) c[i] = (int)s->p.border[i]; }
static void nvnSamplerGetBorderColorui(const samp_t *s, uint32_t *c) { if (s && c) for (int i = 0; i < 4; i++) c[i] = (uint32_t)s->p.border[i]; }
static float nvnSamplerGetMaxAnisotropy(const samp_t *s) { return s ? s->p.aniso : 0; }
static int nvnSamplerGetReductionFilter(const samp_t *s) { return s ? s->p.reduction : 0; }
static uint32_t nvnSamplerCompare(const samp_t *a, const samp_t *b) {
    return a && b && !memcmp(&a->p, &b->p, sizeof a->p);
}
static uint64_t nvnSamplerGetDebugID(const samp_t *s) { return s ? (uint64_t)(uintptr_t)s : 0; }

/* Pools of descriptors. */
static void nvn_tex_pool_forget(const tex_pool_t *p) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_tex_pool_map_count; i++)
        if (g_tex_pool_map[i].pool != p) g_tex_pool_map[n++] = g_tex_pool_map[i];
    g_tex_pool_map_count = n;
}
static void nvn_tex_pool_add(const tex_pool_t *p, int id, const tex_t *t) {
    for (unsigned i = 0; i < g_tex_pool_map_count; i++)
        if (g_tex_pool_map[i].pool == p && g_tex_pool_map[i].id == id) {
            g_tex_pool_map[i].tex = t;
            return;
        }
    if (g_tex_pool_map_count < sizeof g_tex_pool_map / sizeof g_tex_pool_map[0]) {
        g_tex_pool_map[g_tex_pool_map_count++] = (tex_pool_map_t){ p, id, t };
    } else {
        static int warned;
        if (!warned++) fprintf(stderr, "[totk-nvn] ERROR texture pool map full (%u registrations); later binds resolve NULL\n",
                               g_tex_pool_map_count);
    }
}
static const tex_t *nvn_tex_pool_lookup(const tex_pool_t *p, int id) {
    for (unsigned i = 0; i < g_tex_pool_map_count; i++)
        if (g_tex_pool_map[i].pool == p && g_tex_pool_map[i].id == id)
            return g_tex_pool_map[i].tex;
    return NULL;
}
static void nvn_samp_pool_forget(const samp_pool_t *p) {
    unsigned n = 0;
    for (unsigned i = 0; i < g_samp_pool_map_count; i++)
        if (g_samp_pool_map[i].pool != p) g_samp_pool_map[n++] = g_samp_pool_map[i];
    g_samp_pool_map_count = n;
}
static void nvn_samp_pool_add(const samp_pool_t *p, int id, const samp_t *s) {
    for (unsigned i = 0; i < g_samp_pool_map_count; i++)
        if (g_samp_pool_map[i].pool == p && g_samp_pool_map[i].id == id) {
            g_samp_pool_map[i].samp = s;
            return;
        }
    if (g_samp_pool_map_count < sizeof g_samp_pool_map / sizeof g_samp_pool_map[0]) {
        g_samp_pool_map[g_samp_pool_map_count++] = (samp_pool_map_t){ p, id, s };
    } else {
        static int warned;
        if (!warned++) fprintf(stderr, "[totk-nvn] ERROR sampler pool map full (%u registrations); later binds resolve NULL\n",
                               g_samp_pool_map_count);
    }
}
static const samp_t *nvn_samp_pool_lookup(const samp_pool_t *p, int id) {
    for (unsigned i = 0; i < g_samp_pool_map_count; i++)
        if (g_samp_pool_map[i].pool == p && g_samp_pool_map[i].id == id)
            return g_samp_pool_map[i].samp;
    return NULL;
}

static uint32_t nvnTexturePoolInitialize(tex_pool_t *p, const pool_t *pool, ptrdiff_t off, int n) { nvn_api("nvnTexturePoolInitialize");
    if (!p || !nvn_obj(pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnTexturePoolInitialize(pool)")) return 0;
    if (n < 0) { nvn_fail("nvnTexturePoolInitialize", "negative count"); return 0; }
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('T', 'P', 'O', 'L'); p->pool = (pool_t *)pool; p->off = off; p->n = (uint32_t)n;
    return 1;
}
static void nvnTexturePoolFinalize(tex_pool_t *p) { if (p) { nvn_tex_pool_forget(p); p->magic = 0; } }
static void nvnTexturePoolSetDebugLabel(tex_pool_t *p, const char *l) { (void)p; (void)l; }
static void nvnTexturePoolRegisterTexture(const tex_pool_t *p, int id, const tex_t *t, const void *v) { nvn_api("nvnTexturePoolRegisterTexture");
    (void)v;
    if (!p || p->magic != NVN_MAGIC('T', 'P', 'O', 'L') || !nvn_obj(t, NVN_MAGIC('T', 'E', 'X', 0), "nvnTexturePoolRegisterTexture(texture)")) return;
    if (id < 0 || (uint32_t)id >= p->n) { nvn_fail("nvnTexturePoolRegisterTexture", "id out of range"); return; }
    nvn_tex_pool_add(p, id, t);
}
static void nvnTexturePoolRegisterImage(const tex_pool_t *p, int id, const tex_t *t, const void *v) {
    nvnTexturePoolRegisterTexture(p, id, t, v);
}
static void *nvnTexturePoolGetMemoryPool(const tex_pool_t *p) { return p ? p->pool : NULL; }
static int64_t nvnTexturePoolGetMemoryOffset(const tex_pool_t *p) { return p ? p->off : 0; }
static int nvnTexturePoolGetSize(const tex_pool_t *p) { return p ? (int)p->n : 0; }

static uint32_t nvnSamplerPoolInitialize(samp_pool_t *p, const pool_t *pool, ptrdiff_t off, int n) { nvn_api("nvnSamplerPoolInitialize");
    if (!p || !nvn_obj(pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnSamplerPoolInitialize(pool)")) return 0;
    if (n < 0) { nvn_fail("nvnSamplerPoolInitialize", "negative count"); return 0; }
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('S', 'P', 'O', 'L'); p->pool = (pool_t *)pool; p->off = off; p->n = (uint32_t)n;
    return 1;
}
static void nvnSamplerPoolFinalize(samp_pool_t *p) { if (p) { nvn_samp_pool_forget(p); p->magic = 0; } }
static void nvnSamplerPoolSetDebugLabel(samp_pool_t *p, const char *l) { (void)p; (void)l; }
static void nvnSamplerPoolRegisterSampler(const samp_pool_t *p, int id, const samp_t *s) { nvn_api("nvnSamplerPoolRegisterSampler");
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
static void nvnSamplerPoolRegisterSamplerBuilder(const samp_pool_t *p, int id, const samp_builder_t *b) {
    if (!p || !b) { nvn_fail("nvnSamplerPoolRegisterSamplerBuilder", "null pool/builder"); return; }
    if (id < 0 || (uint32_t)id >= p->n) { nvn_fail("nvnSamplerPoolRegisterSamplerBuilder", "id out of range"); return; }
}
static void *nvnSamplerPoolGetMemoryPool(const samp_pool_t *p) { return p ? p->pool : NULL; }
static int64_t nvnSamplerPoolGetMemoryOffset(const samp_pool_t *p) { return p ? p->off : 0; }
static int nvnSamplerPoolGetSize(const samp_pool_t *p) { return p ? (int)p->n : 0; }

/* ------------------------------------------------------------------ */
/* Command buffer                                                      */

typedef struct {
    uint32_t magic;
    uint32_t recording;
    void *dev;
    void *ctl;
    size_t ctl_size, ctl_used;
    void *cmd;
    size_t cmd_size, cmd_used;
    void *mem_cb, *mem_cb_data;
    void *prog;
    void *win;
    const tex_pool_t *tex_pool;
    const samp_pool_t *samp_pool;
    uint64_t serial, handle;
    int draw_first, draw_count;
    nvn_cmd_t *cmds;
    uint32_t ncmds, cmds_cap;
    void *bind_state;
} cb_t;

_Static_assert(sizeof(cb_t) <= NVN_COMMAND_BUFFER_SIZE, "command buffer object too large");
static uint64_t nvn_cb_serial;
static cb_t **g_cbs;
static unsigned g_ncbs, g_cbs_cap;
static unsigned nvn_nvn_draws;

/* --- Host render targets (MRT + depth) ------------------------------ */
/* NVN binds a set of color targets plus depth; the host mirrors that with a
 * GfxOffscreenTarget created for the complete layout.  Targets are cached by
 * (size, layout) so a repeated frame reuses one image instead of churning
 * Vulkan objects. */
typedef struct {
    uint32_t width, height;
    GfxRenderTargetLayout layout;
    GfxOffscreenTarget *gfx;
} nvn_host_rt_t;
static nvn_host_rt_t g_host_rts[16];
static unsigned g_host_rt_count;
static pthread_mutex_t g_host_rt_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t nvn_layout_sig(const GfxRenderTargetLayout *l) {
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
static int nvn_layout_equal(const GfxRenderTargetLayout *a, const GfxRenderTargetLayout *b) {
    if (!a || !b || a->color_count != b->color_count || a->depth_format != b->depth_format)
        return 0;
    for (uint32_t i = 0; i < a->color_count && i < GFX_MAX_COLOR_TARGETS; i++)
        if (a->color_formats[i] != b->color_formats[i]) return 0;
    return 1;
}
static int nvn_layout_valid(const GfxRenderTargetLayout *l) {
    if (!l || l->color_count < 1 || l->color_count > GFX_MAX_COLOR_TARGETS) return 0;
    for (uint32_t i = 0; i < l->color_count; i++)
        if (!l->color_formats[i]) return 0;
    return 1;
}
static GfxOffscreenTarget *nvn_host_rt_get(uint32_t width, uint32_t height,
                                           const GfxRenderTargetLayout *layout) {
    if (!width || !height || !nvn_layout_valid(layout)) return NULL;
    pthread_mutex_lock(&g_host_rt_lock);
    for (unsigned i = 0; i < g_host_rt_count; i++) {
        if (g_host_rts[i].width != width || g_host_rts[i].height != height) continue;
        if (!nvn_layout_equal(&g_host_rts[i].layout, layout)) continue;
        GfxOffscreenTarget *gfx = g_host_rts[i].gfx;
        pthread_mutex_unlock(&g_host_rt_lock);
        return gfx;
    }
    if (g_host_rt_count >= sizeof g_host_rts / sizeof g_host_rts[0]) {
        pthread_mutex_unlock(&g_host_rt_lock);
        nvn_fail("nvnCommandBufferSetRenderTargets", "host render-target cache full");
        return NULL;
    }
    GfxOffscreenTarget *gfx = gfx_backend_offscreen_create_layout(width, height, layout);
    if (!gfx) {
        pthread_mutex_unlock(&g_host_rt_lock);
        return NULL;
    }
    g_host_rts[g_host_rt_count].width = width;
    g_host_rts[g_host_rt_count].height = height;
    g_host_rts[g_host_rt_count].layout = *layout;
    g_host_rts[g_host_rt_count].gfx = gfx;
    g_host_rt_count++;
    if (g_host_rt_count <= 8 || nvn_trace())
        fprintf(stderr, "[totk-nvn] host render target %ux%u colors=%u depth=%#x created\n",
                width, height, layout->color_count, layout->depth_format);
    pthread_mutex_unlock(&g_host_rt_lock);
    return gfx;
}
static void nvn_host_rt_clear(void) {
    pthread_mutex_lock(&g_host_rt_lock);
    for (unsigned i = 0; i < g_host_rt_count; i++) {
        if (g_host_rts[i].gfx) gfx_backend_offscreen_destroy(g_host_rts[i].gfx);
        g_host_rts[i].gfx = NULL;
    }
    g_host_rt_count = 0;
    pthread_mutex_unlock(&g_host_rt_lock);
}

/* Per-command-buffer resolved render-target state.  Kept out of cb_t so the
 * guest's NVN_COMMAND_BUFFER_SIZE contract does not change. */
typedef struct {
    const cb_t *cb;
    GfxRenderTargetLayout layout;
    uint32_t width, height;
    int valid;
    int window_texture;   /* color 0 is a registered window swapchain texture */
} nvn_cb_target_t;
static nvn_cb_target_t g_cb_targets[64];
static pthread_mutex_t g_cb_target_lock = PTHREAD_MUTEX_INITIALIZER;

static nvn_cb_target_t *nvn_cb_target(const cb_t *cb, int create) {
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
static void nvn_cb_target_forget(const cb_t *cb) {
    nvn_cb_target_t *slot = nvn_cb_target(cb, 0);
    if (!slot) return;
    pthread_mutex_lock(&g_cb_target_lock);
    memset(slot, 0, sizeof *slot);
    pthread_mutex_unlock(&g_cb_target_lock);
}

/* Resolve the NVN texture set to a host layout.  Every color target and the
 * depth target contribute their guest dimensions/format; unsupported formats
 * are explicit failures (the backend returns VK_FORMAT_UNDEFINED = 0). */
static int nvn_rt_layout_from_targets(uint32_t n, void *const *tex, const void *depth,
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
    for (uint32_t i = 0; i < n; i++) {
        const tex_t *t = nvn_tex_registry_find(tex ? tex[i] : NULL);
        if (!t) { if (why) *why = "color target is not an NVN texture"; return 0; }
        int fmt = gfx_backend_vk_format_for_nvn((uint32_t)t->d.format);
        if (!fmt) {
            if (why) *why = "unsupported color target format";
            return 0;
        }
        layout->color_formats[i] = fmt;
        if (i == 0) { *width = t->d.width; *height = t->d.height; }
        else if (t->d.width != *width || t->d.height != *height) {
            static unsigned logged;
            if (logged++ < 8)
                fprintf(stderr, "[totk-nvn] WARNING color target %u is %ux%u, color0 is %ux%u\n",
                        i, t->d.width, t->d.height, *width, *height);
        }
    }
    layout->color_count = n;
    if (depth) {
        const tex_t *dt = nvn_tex_registry_find(depth);
        if (!dt) { if (why) *why = "depth target is not an NVN texture"; return 0; }
        int fmt = gfx_backend_vk_format_for_nvn((uint32_t)dt->d.format);
        if (!fmt) { if (why) *why = "unsupported depth target format"; return 0; }
        layout->depth_format = fmt;
        if (!*width) { *width = dt->d.width; *height = dt->d.height; }
    }
    if (!*width || !*height) { if (why) *why = "render target has no dimensions"; return 0; }
    return 1;
}

/* The window's host target layout comes from the guest window texture when the
 * guest supplied one; only a bare window falls back to the VI description. */
static void nvn_window_layout(const win_t *w, GfxRenderTargetLayout *layout,
                              uint32_t *width, uint32_t *height) {
    memset(layout, 0, sizeof *layout);
    uint32_t ww = 0, wh = 0, wf = 0;
    *width = *height = 0;
    if (w && w->native && runtime_native_window_info(w->native, &ww, &wh, &wf, NULL, NULL)) {
        *width = ww; *height = wh;
    }
    uint32_t fmt = gfx_backend_vk_format_for_nvn(wf ? wf : 0x25);
    if (!fmt) fmt = gfx_backend_vk_format_for_nvn(0x25);
    layout->color_count = 1;
    layout->color_formats[0] = fmt;
    layout->depth_format = 0;
}


static int nvn_cb_track(cb_t *c) {
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
static void nvn_cb_untrack(const cb_t *c) {
    for (unsigned i = 0; i < g_ncbs; i++) {
        if (g_cbs[i] != c) continue;
        g_cbs[i] = g_cbs[--g_ncbs];
        g_cbs[g_ncbs] = NULL;
        return;
    }
}

static nvn_cmd_t *nvn_cb_cmd(cb_t *c, uint32_t kind) {
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

static nvn_bind_state_t *nvn_cb_bind_state(cb_t *c) {
    if (!c) return NULL;
    if (!c->bind_state) {
        c->bind_state = calloc(1, sizeof(nvn_bind_state_t));
        if (c->bind_state) nvn_render_state_defaults(&((nvn_bind_state_t *)c->bind_state)->rs);
    }
    return c->bind_state;
}

static void nvn_cb_state_event(cb_t *c) {
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_BIND_STATE);
    if (cmd) cmd->u.state.rs = b->rs;
}

/* Descriptor events are recorded in command order so the backend can give
 * each draw its own descriptor epoch.  kind: 0 UBO, 1 SSBO, 2 texture. */
static void nvn_cb_desc_event(cb_t *c, uint32_t stage, uint32_t kind, uint32_t slot,
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

static uint32_t nvnCommandBufferInitialize(cb_t *c, void *d) { nvn_api("nvnCommandBufferInitialize");
    if (!c) { nvn_fail("nvnCommandBufferInitialize", "null command buffer"); return 0; }
    /* Re-initialising the same guest object must drop the previous recording
     * and registry entry, not leak them or leave a stale handle resolvable. */
    for (unsigned i = 0; i < g_ncbs; i++) {
        if (g_cbs[i] != c) continue;
        nvn_cb_untrack(c);
        if (c->magic == NVN_MAGIC('C', 'M', 'D', 'B')) {
            free(c->cmds);
            free(c->bind_state);
        }
        break;
    }
    memset(c, 0, sizeof *c);
    nvn_cb_target_forget(c);
    c->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    c->dev = d;
    c->serial = ++nvn_cb_serial;
    if (!nvn_cb_track(c)) {
        c->magic = 0;
        nvn_fail("nvnCommandBufferInitialize", "out of memory for command-buffer registry");
        return 0;
    }
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] CommandBufferInitialize %p\n", (void *)c);
    return 1;
}
static void nvnCommandBufferFinalize(cb_t *c) {
    if (!c) return;
    nvn_cb_untrack(c);
    nvn_cb_target_forget(c);
    free(c->cmds);
    free(c->bind_state);
    c->cmds = NULL;
    c->bind_state = NULL;
    c->ncmds = c->cmds_cap = 0;
    c->prog = NULL;
    c->win = NULL;
    c->draw_first = c->draw_count = 0;
    c->recording = 0;
    c->handle = 0;
    c->magic = 0;
}
static void nvnCommandBufferSetDebugLabel(cb_t *c, const char *l) { (void)c; (void)l; }
static void nvnCommandBufferSetMemoryCallback(cb_t *c, void *cb) { if (c) c->mem_cb = cb; }
static void nvnCommandBufferSetMemoryCallbackData(cb_t *c, void *d) { if (c) c->mem_cb_data = d; }
static void nvnCommandBufferSetControlMemoryCallbackEnabled(cb_t *c, int e) { (void)c; (void)e; }
static void nvnCommandBufferSetCommandMemoryCallbackEnabled(cb_t *c, int e) { (void)c; (void)e; }
static void nvnCommandBufferAddCommandMemory(cb_t *c, const pool_t *p, ptrdiff_t off, size_t n) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferAddCommandMemory", "invalid command buffer"); return; }
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnCommandBufferAddCommandMemory(pool)")) return;
    if (!p->mem) { nvn_fail("nvnCommandBufferAddCommandMemory", "pool has no memory"); return; }
    c->cmd = p->mem + off;
    c->cmd_size = n;
    c->cmd_used = 0;
}
static void nvnCommandBufferAddControlMemory(cb_t *c, void *m, size_t n) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferAddControlMemory", "invalid command buffer"); return; }
    c->ctl = m;
    c->ctl_size = n;
    c->ctl_used = 0;
}
static size_t nvnCommandBufferGetControlMemorySize(const cb_t *c) { return c ? c->ctl_size : 0; }
static size_t nvnCommandBufferGetControlMemoryUsed(const cb_t *c) { return c ? c->ctl_used : 0; }
static size_t nvnCommandBufferGetCommandMemorySize(const cb_t *c) { return c ? c->cmd_size : 0; }
static size_t nvnCommandBufferGetCommandMemoryUsed(const cb_t *c) { return c ? c->cmd_used : 0; }
static void nvnCommandBufferBeginRecording(cb_t *c) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBeginRecording", "invalid command buffer"); return; }
    c->recording = 1;
    c->cmd_used = 0;
    c->ctl_used = 0;
    c->draw_first = 0;
    c->draw_count = 0;
    c->ncmds = 0;
    nvn_cb_target_forget(c);
    /* A fresh recording starts with default (unbound) resource/vertex state;
     * keeping the previous recording's state would re-apply stale guest
     * pointers on the next submit.  Render state, however, starts from the NVN
     * defaults (not all-zero: a zero channel-mask would disable colour writes). */
    if (c->bind_state) {
        nvn_bind_state_t *b = c->bind_state;
        memset(b, 0, sizeof *b);
        nvn_render_state_defaults(&b->rs);
    }
}
static uint64_t nvnCommandBufferEndRecording(cb_t *c) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferEndRecording", "invalid command buffer"); return 0; }
    c->recording = 0;
    uint64_t h = 0x100000000ull | (c->serial & 0xffffffffull);
    c->handle = h;
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] CommandBufferEndRecording %p -> handle %#llx (%u cmds)\n",
                             (void *)c, (unsigned long long)h, c->ncmds);
    return h;
}
static void nvnCommandBufferBindProgram(cb_t *c, const prog_t *p, int stage_mask) {
    nvn_api("nvnCommandBufferBindProgram");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindProgram", "invalid command buffer"); return; }
    if (!nvn_obj(p, NVN_MAGIC('P', 'R', 'O', 'G'), "nvnCommandBufferBindProgram(program)")) return;
    c->prog = (void *)p;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindProgram program=%p stageMask=%#x\n", (const void *)p, stage_mask);
}
/* Host bridge used by the self-tests; the guest NVN API is BindProgram. */
static void nvnCommandBufferSetProgram(cb_t *c, prog_t *p) {
    nvnCommandBufferBindProgram(c, p, -1);
}
static void nvnCommandBufferSetWindow(cb_t *c, win_t *w) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetWindow", "invalid command buffer"); return; }
    if (!nvn_obj(w, NVN_MAGIC('W', 'I', 'N', 'D'), "nvnCommandBufferSetWindow(window)")) return;
    c->win = w;
}
static void nvnCommandBufferDrawArrays(cb_t *c, int mode, int first, int count) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferDrawArrays", "invalid command buffer");
        return;
    }
    if (count < 0 || first < 0) {
        nvn_fail("nvnCommandBufferDrawArrays", "negative first/count");
        return;
    }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW);
    if (cmd) {
        cmd->u.draw.primitive = (uint32_t)mode;
        cmd->u.draw.first = (uint32_t)first;
        cmd->u.draw.count = (uint32_t)count;
        cmd->u.draw.instances = 1;
        cmd->u.draw.base_instance = 0;
    }
    c->draw_first = first;
    c->draw_count += count;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] CommandBufferDrawArrays mode=%d first=%d count=%d\n",
                mode, first, count);
}
static void nvnCommandBufferDrawArraysInstanced(cb_t *c, int mode, int first, int count,
                                                int base_instance, int instances) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) {
        nvn_fail("nvnCommandBufferDrawArraysInstanced", "invalid command buffer");
        return;
    }
    if (count < 0 || first < 0 || base_instance < 0 || instances < 0) {
        nvn_fail("nvnCommandBufferDrawArraysInstanced", "negative argument");
        return;
    }
    if (instances == 0) return;   /* NVN: zero instances draw nothing */
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_DRAW);
    if (cmd) {
        cmd->u.draw.primitive = (uint32_t)mode;
        cmd->u.draw.first = (uint32_t)first;
        cmd->u.draw.count = (uint32_t)count;
        cmd->u.draw.instances = (uint32_t)instances;
        cmd->u.draw.base_instance = (uint32_t)base_instance;
    }
    c->draw_first = first;
    c->draw_count += count;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] CommandBufferDrawArraysInstanced mode=%d first=%d count=%d base=%d instances=%d\n",
                mode, first, count, base_instance, instances);
}
static void nvnCommandBufferSetRenderEnable(cb_t *c, int e) { (void)c; (void)e; }
#define NVN_ST(T, x) ((T *)(x))

static void nvnBlendStateSetDefaults(void *s) {
    if (s) *(nvn_blend_st *)s = (nvn_blend_st){ 0, 2, 1, 2, 1, 1, 1 };
}
static void nvnBlendStateSetBlendTarget(void *s, int t) { if (s) ((nvn_blend_st *)s)->target = (uint32_t)t; }
static void nvnBlendStateSetBlendFunc(void *s, int sr, int dr, int sa, int da) {
    if (!s) return;
    nvn_blend_st *o = s;
    o->src_rgb = (uint32_t)sr; o->dst_rgb = (uint32_t)dr;
    o->src_a = (uint32_t)sa; o->dst_a = (uint32_t)da;
}
static void nvnBlendStateSetBlendEquation(void *s, int er, int ea) {
    if (!s) return;
    ((nvn_blend_st *)s)->eq_rgb = (uint32_t)er;
    ((nvn_blend_st *)s)->eq_a = (uint32_t)ea;
}
static void nvnColorStateSetDefaults(void *s) {
    if (s) *(nvn_color_st *)s = (nvn_color_st){ 0, 3, 8 };
}
static void nvnColorStateSetBlendEnable(void *s, int i, uint32_t en) {
    if (!s || i < 0 || i > 7) return;
    nvn_color_st *o = s;
    if (en) o->blend_en |= 1u << i; else o->blend_en &= ~(1u << i);
}
static void nvnChannelMaskStateSetDefaults(void *s) { if (s) ((nvn_chmask_st *)s)->mask = 0xffffffffu; }
static void nvnChannelMaskStateSetChannelMask(void *s, int i, uint32_t r, uint32_t g,
                                              uint32_t b, uint32_t a) {
    if (!s || i < 0 || i > 7) return;
    uint32_t m = (r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u);
    nvn_chmask_st *o = s;
    o->mask = (o->mask & ~(0xfu << (4 * i))) | (m << (4 * i));
}
static void nvnMultisampleStateSetDefaults(void *s) {
    if (!s) return;
    memset(s, 0, sizeof(nvn_ms_st));
    ((nvn_ms_st *)s)->enable = 1;
}
static void nvnMultisampleStateSetMultisampleEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->enable = e != 0;
}
static void nvnMultisampleStateSetSamples(void *s, int n) {
    if (s) ((nvn_ms_st *)s)->samples = (uint8_t)n;
}
static void nvnMultisampleStateSetAlphaToCoverageEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->a2c = e != 0;
}
static void nvnMultisampleStateSetSampleLocationsEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->sl_en = e != 0;
}
static void nvnMultisampleStateSetSampleLocations(void *s, int start, int n, const float *v) {
    (void)s; (void)start; (void)n; (void)v;
}
static void nvnPolygonStateSetDefaults(void *s) {
    if (s) *(nvn_poly_st *)s = (nvn_poly_st){ 0, 1, 2, 0 };
}
static void nvnPolygonStateSetCullFace(void *s, int f) { if (s) ((nvn_poly_st *)s)->cull = (uint32_t)f; }
static void nvnPolygonStateSetFrontFace(void *s, int f) { if (s) ((nvn_poly_st *)s)->front = (uint32_t)f; }
static void nvnPolygonStateSetPolygonMode(void *s, int m) { if (s) ((nvn_poly_st *)s)->mode = (uint32_t)m; }
static void nvnPolygonStateSetPolygonOffsetEnables(void *s, int e) {
    if (s) ((nvn_poly_st *)s)->offset_en = (uint32_t)e;
}
static void nvnDepthStencilStateSetDefaults(void *s) {
    if (!s) return;
    *(nvn_ds_st *)s = (nvn_ds_st){ .dt = 0, .dw = 1, .df = 2, .st = 0,
        .sf_f = 8, .sf_b = 8, .fail_f = 1, .zfail_f = 1, .zpass_f = 1,
        .fail_b = 1, .zfail_b = 1, .zpass_b = 1 };
}
static void nvnDepthStencilStateSetDepthTestEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->dt = e != 0; }
static void nvnDepthStencilStateSetDepthWriteEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->dw = e != 0; }
static void nvnDepthStencilStateSetDepthFunc(void *s, int f) { if (s) ((nvn_ds_st *)s)->df = (uint32_t)f; }
static void nvnDepthStencilStateSetStencilTestEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->st = e != 0; }
static void nvnDepthStencilStateSetStencilFunc(void *s, int f, int fn) {
    if (!s) return;
    nvn_ds_st *o = s;
    if (f & 1) o->sf_f = (uint32_t)fn;
    if (f & 2) o->sf_b = (uint32_t)fn;
}
static void nvnDepthStencilStateSetStencilOp(void *s, int f, int fail, int zfail, int zpass) {
    if (!s) return;
    nvn_ds_st *o = s;
    if (f & 1) { o->fail_f = (uint32_t)fail; o->zfail_f = (uint32_t)zfail; o->zpass_f = (uint32_t)zpass; }
    if (f & 2) { o->fail_b = (uint32_t)fail; o->zfail_b = (uint32_t)zfail; o->zpass_b = (uint32_t)zpass; }
}

static void nvnCommandBufferBindBlendState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.blend = *(const nvn_blend_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferBindColorState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.color = *(const nvn_color_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferBindChannelMaskState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.chmask = *(const nvn_chmask_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferBindMultisampleState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.ms = *(const nvn_ms_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferBindPolygonState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.poly = *(const nvn_poly_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferBindDepthStencilState(cb_t *c, const void *s) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !s) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.ds = *(const nvn_ds_st *)s; nvn_cb_state_event(c); }
}
static void nvnCommandBufferSetBlendColor(cb_t *c, const float *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B') || !v) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { memcpy(b->rs.blend_color, v, sizeof b->rs.blend_color); nvn_cb_state_event(c); }
}
static void nvnCommandBufferSetStencilRef(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_ref = v; nvn_cb_state_event(c); }
}
static void nvnCommandBufferSetStencilMask(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_mask = v; nvn_cb_state_event(c); }
}
static void nvnCommandBufferSetStencilValueMask(cb_t *c, int face, int v) {
    (void)face;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) { b->rs.stencil_value_mask = v; nvn_cb_state_event(c); }
}
static void nvnCommandBufferClearDepthStencil(cb_t *c, float depth, uint32_t dmask,
                                              int stencil, int smask) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return;
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_CLEAR_DEPTH_STENCIL);
    if (cmd) {
        cmd->u.clear_ds.depth = depth;
        cmd->u.clear_ds.dmask = dmask;
        cmd->u.clear_ds.stencil = stencil;
        cmd->u.clear_ds.smask = (uint32_t)smask;
    }
}

/* First-frame diagnostics: one line per bound target/binding with the NVN
 * descriptor fields needed to describe the frame (format, kind, size, layers,
 * mips, samples).  Only runs with TOTK_ANDROID_TRACE_NVN. */
__attribute__((unused)) static void nvn_trace_tex(const char *what, const tex_t *t) {
    if (!t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) {
        fprintf(stderr, "[totk-nvn] %s texture=NULL\n", what);
        return;
    }
    fprintf(stderr, "[totk-nvn] %s texture=%p target=%d format=%#x %ux%ux%u layers=%u mips=%u samples=%d\n",
            what, (const void *)t, t->d.target, t->d.format, t->d.width, t->d.height,
            t->d.depth ? t->d.depth : 1u, t->has_layout ? t->L.layers : 1u,
            t->d.levels, t->d.samples);
}
static void nvnCommandBufferSetRenderTargets(cb_t *c, int n, const void *const *t,
                                             const void *const *views,
                                             const void *depth, const void *depth_view) {
    nvn_api("nvnCommandBufferSetRenderTargets");
    (void)views; (void)depth_view;
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetRenderTargets", "invalid command buffer"); return; }
    nvn_bind_state_t *bs = nvn_cb_bind_state(c);
    if (bs) bs->depth_tex = depth;
    if (n < 0 || n > GFX_MAX_COLOR_TARGETS || (!t && n)) { nvn_fail("nvnCommandBufferSetRenderTargets", "invalid target count"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_RENDER_TARGETS);
    if (cmd) {
        cmd->u.targets.count = (uint32_t)n;
        for (int i = 0; i < n; i++) cmd->u.targets.tex[i] = (void *)t[i];
        cmd->u.targets.depth = (void *)depth;
    }
    /* Resolve the complete set now: every color target and the depth target
     * keep their own guest format/dimensions for the layout and pipeline key. */
    nvn_cb_target_t *cbt = nvn_cb_target(c, 1);
    if (cbt) {
        cbt->valid = 0;
        cbt->width = cbt->height = 0;
        memset(&cbt->layout, 0, sizeof cbt->layout);
        if (n > 0) {
            const char *why = NULL;
            if (nvn_rt_layout_from_targets((uint32_t)n, (void *const *)t, depth,
                                           &cbt->layout, &cbt->width, &cbt->height, &why)) {
                cbt->valid = 1;
            } else {
                char d[128];
                snprintf(d, sizeof d, "cannot resolve render targets: %s", why ? why : "unknown");
                nvn_fail("nvnCommandBufferSetRenderTargets", d);
            }
        }
    }
    /* NVN binds textures, not windows.  Resolve the window whose swapchain
     * texture is color target 0 so present/draw share one host target. */
    if (n > 0 && t && t[0]) {
        win_t *w = nvn_window_for_texture(t[0]);
        if (w) { c->win = w; if (cbt) cbt->window_texture = 1; }
    }
    if (nvn_trace() && n) {
        const tex_t *t0 = nvn_tex_registry_find(t[0]);
        const tex_t *dt = nvn_tex_registry_find(depth);
        fprintf(stderr, "[totk-nvn] SetRenderTargets count=%d window=%p", n, (void *)c->win);
        for (int i = 0; i < n; i++) {
            const tex_t *ti = nvn_tex_registry_find(t[i]);
            fprintf(stderr, " color%d=%p(%s fmt=%#x %ux%u)", i, t[i], ti ? "tex" : "?",
                    ti ? ti->d.format : 0, ti ? ti->d.width : 0, ti ? ti->d.height : 0);
        }
        fprintf(stderr, " depth=%p(tex=%d fmt=%#x %ux%u)\n", depth, dt != NULL,
                dt ? dt->d.format : 0, dt ? dt->d.width : 0, dt ? dt->d.height : 0);
        (void)t0;
    }

}
static void nvnCommandBufferClearColor(cb_t *c, int i, const float *v, int mask) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferClearColor", "invalid command buffer"); return; }
    if (i < 0 || i > 7 || !v) { nvn_fail("nvnCommandBufferClearColor", "invalid target/color"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_CLEAR_COLOR);
    if (cmd) {
        memcpy(cmd->u.clear.rgba, v, sizeof cmd->u.clear.rgba);
        cmd->u.clear.index = i;
        cmd->u.clear.mask = (uint32_t)mask;
    }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] ClearColor target=%d rgba=%.3f,%.3f,%.3f,%.3f mask=%#x\n",
                i, v[0], v[1], v[2], v[3], mask);
}
static void nvnCommandBufferClearColori(cb_t *c, int i, const int *v, int mask) {
    if (!v) { nvn_fail("nvnCommandBufferClearColori", "null color"); return; }
    const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] };
    nvnCommandBufferClearColor(c, i, f, mask);
}
static void nvnCommandBufferClearColorui(cb_t *c, int i, const uint32_t *v, int mask) {
    if (!v) { nvn_fail("nvnCommandBufferClearColorui", "null color"); return; }
    const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] };
    nvnCommandBufferClearColor(c, i, f, mask);
}
static void nvnCommandBufferSetViewport(cb_t *c, int x, int y, int w, int h) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetViewport", "invalid command buffer"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_VIEWPORT);
    if (cmd) { cmd->u.rect.x = x; cmd->u.rect.y = y; cmd->u.rect.width = w; cmd->u.rect.height = h; }
}
static void nvnCommandBufferSetScissor(cb_t *c, int x, int y, int w, int h) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetScissor", "invalid command buffer"); return; }
    nvn_cmd_t *cmd = nvn_cb_cmd(c, NVN_CMD_SET_SCISSOR);
    if (cmd) { cmd->u.rect.x = x; cmd->u.rect.y = y; cmd->u.rect.width = w; cmd->u.rect.height = h; }
}
static void nvnCommandBufferSetViewports(cb_t *c, int first, int n, const float *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetViewports", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || (!v && n) || !n) return;
    /* NVNviewport array: x, y, width, height, near, far (six floats each). */
    nvnCommandBufferSetViewport(c, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
}
static void nvnCommandBufferSetScissors(cb_t *c, int first, int n, const int *v) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetScissors", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || (!v && n) || !n) return;
    nvnCommandBufferSetScissor(c, v[0], v[1], v[2], v[3]);
}

static void nvnCommandBufferBindVertexBuffer(cb_t *c, int index, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindVertexBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindVertexBuffer", "invalid command buffer"); return; }
    if (index < 0 || index >= 16 || !address || !size) { nvn_fail("nvnCommandBufferBindVertexBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    b->vb[index].address = address;
    b->vb[index].size = size;
    b->vb_mask |= 1u << index;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindVertexBuffer index=%d addr=%#llx size=%zu\n",
                index, (unsigned long long)address, size);
}
static void nvnCommandBufferBindVertexBuffers(cb_t *c, int first, int n, const void *ranges) {
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
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindVertexBuffers first=%d count=%d\n", first, n);
}
static void nvnCommandBufferBindVertexAttribState(cb_t *c, int n, const void *states) {
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
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindVertexAttribState count=%d\n", n);
}
static void nvnCommandBufferBindVertexStreamState(cb_t *c, int n, const void *states) {
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
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindVertexStreamState count=%d\n", n);
}
static void nvnCommandBufferSetPrimitiveRestart(cb_t *c, uint32_t enable, int index) {
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetPrimitiveRestart", "invalid command buffer"); return; }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] SetPrimitiveRestart enable=%u index=%d\n", enable, index);
}
static void nvnCommandBufferDrawElements(cb_t *c, int mode, int type, int count,
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
    }
    c->draw_count += count;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] DrawElements mode=%d type=%d count=%d index=%#llx\n",
                mode, type, count, (unsigned long long)index);
}
static void nvnCommandBufferDrawElementsBaseVertex(cb_t *c, int mode, int type, int count,
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
    }
    c->draw_count += count;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] DrawElementsBaseVertex mode=%d type=%d count=%d index=%#llx base=%d\n",
                mode, type, count, (unsigned long long)index, base_vertex);
}

/* NVN descriptor state.  The shader reflection names a NVN slot (cbufs/sbufs)
 * or a c[2] handle (textures/images); these calls are the source of the actual
 * resources.  Drawing still uses the host pipeline until the guest reaches the
 * nn::gfx context path, but the state is kept and traced so a future bind is
 * not another guess. */
static void nvnCommandBufferSetTexturePool(cb_t *c, const tex_pool_t *p) {
    nvn_api("nvnCommandBufferSetTexturePool");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetTexturePool", "invalid command buffer"); return; }
    if (p && p->magic != NVN_MAGIC('T', 'P', 'O', 'L')) { nvn_fail("nvnCommandBufferSetTexturePool", "invalid texture pool"); return; }
    c->tex_pool = p;
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] SetTexturePool cb=%p pool=%p n=%d\n", (void *)c, (const void *)p, p ? (int)p->n : 0);
}
static void nvnCommandBufferSetSamplerPool(cb_t *c, const samp_pool_t *p) {
    nvn_api("nvnCommandBufferSetSamplerPool");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferSetSamplerPool", "invalid command buffer"); return; }
    if (p && p->magic != NVN_MAGIC('S', 'P', 'O', 'L')) { nvn_fail("nvnCommandBufferSetSamplerPool", "invalid sampler pool"); return; }
    c->samp_pool = p;
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] SetSamplerPool cb=%p pool=%p n=%d\n", (void *)c, (const void *)p, p ? (int)p->n : 0);
}
static void nvnCommandBufferBindUniformBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindUniformBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindUniformBuffer", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 16 || !address || !size) { nvn_fail("nvnCommandBufferBindUniformBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    b->ubo[stage][i].address = address;
    b->ubo[stage][i].size = size;
    b->ubo_mask[stage] |= 1u << i;
    nvn_cb_desc_event(c, (uint32_t)stage, 0, (uint32_t)i, address, size, 0);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindUniformBuffer stage=%d index=%d addr=%#llx size=%zu\n",
                stage, i, (unsigned long long)address, size);
}
static void nvnCommandBufferBindUniformBuffers(cb_t *c, int stage, int first, int n, const void *ranges) {
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
        nvn_cb_desc_event(c, (uint32_t)stage, 0, (uint32_t)(first + i),
                          r[i * 2], (size_t)r[i * 2 + 1], 0);
    }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindUniformBuffers stage=%d first=%d count=%d\n",
                stage, first, n);
}
static void nvnCommandBufferBindStorageBuffer(cb_t *c, int stage, int i, uint64_t address, size_t size) {
    nvn_api("nvnCommandBufferBindStorageBuffer");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindStorageBuffer", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 16 || !address || !size) { nvn_fail("nvnCommandBufferBindStorageBuffer", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (!b) return;
    b->ssbo[stage][i].address = address;
    b->ssbo[stage][i].size = size;
    b->ssbo_mask[stage] |= 1u << i;
    nvn_cb_desc_event(c, (uint32_t)stage, 1, (uint32_t)i, address, size, 0);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindStorageBuffer stage=%d index=%d addr=%#llx size=%zu\n",
                stage, i, (unsigned long long)address, size);
}
static void nvnCommandBufferBindStorageBuffers(cb_t *c, int stage, int first, int n, const void *ranges) {
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
        nvn_cb_desc_event(c, (uint32_t)stage, 1, (uint32_t)(first + i),
                          r[i * 2], (size_t)r[i * 2 + 1], 0);
    }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindStorageBuffers stage=%d first=%d count=%d\n",
                stage, first, n);
}
static void nvnCommandBufferBindTexture(cb_t *c, int stage, int i, uint64_t handle) {
    nvn_api("nvnCommandBufferBindTexture");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindTexture", "invalid command buffer"); return; }
    if (stage < 0 || stage >= 6 || i < 0 || i >= 128) { nvn_fail("nvnCommandBufferBindTexture", "invalid binding"); return; }
    nvn_bind_state_t *b = nvn_cb_bind_state(c);
    if (b) {
        b->tex[stage][i] = handle;
        if (handle) b->tex_mask[stage][i / 32] |= 1u << (i % 32);
        else b->tex_mask[stage][i / 32] &= ~(1u << (i % 32));
        nvn_cb_desc_event(c, (uint32_t)stage, 2, (uint32_t)i, 0, 0, handle);
    }
    if (nvn_trace()) {
        int tid = (int)(handle & 0xfffff), sid = (int)((handle >> 20) & 0xfff);
        const tex_t *t = c->tex_pool ? nvn_tex_pool_lookup(c->tex_pool, tid) : NULL;
        const samp_t *s = c->samp_pool ? nvn_samp_pool_lookup(c->samp_pool, sid) : NULL;
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
}
static void nvnCommandBufferBindTextures(cb_t *c, int stage, int first, int n, const uint64_t *handles) {
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
        }
    }
    if (nvn_trace()) {
        for (int i = 0; i < n; i++)
            fprintf(stderr, "[totk-nvn] BindTextures stage=%d index=%d handle=%#llx\n",
                    stage, first + i, (unsigned long long)handles[i]);
    }
}
static void nvnCommandBufferBindImage(cb_t *c, int stage, int i, uint64_t handle) {
    nvn_api("nvnCommandBufferBindImage");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindImage", "invalid command buffer"); return; }
    if (i < 0) { nvn_fail("nvnCommandBufferBindImage", "invalid binding"); return; }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] BindImage stage=%d index=%d handle=%#llx\n",
                stage, i, (unsigned long long)handle);
}
static void nvnCommandBufferBindImages(cb_t *c, int stage, int first, int n, const uint64_t *handles) {
    nvn_api("nvnCommandBufferBindImages");
    if (!c || c->magic != NVN_MAGIC('C', 'M', 'D', 'B')) { nvn_fail("nvnCommandBufferBindImages", "invalid command buffer"); return; }
    if (first < 0 || n < 0 || (!handles && n)) { nvn_fail("nvnCommandBufferBindImages", "invalid handles"); return; }
    if (nvn_trace()) {
        for (int i = 0; i < n; i++)
            fprintf(stderr, "[totk-nvn] BindImages stage=%d index=%d handle=%#llx\n",
                    stage, first + i, (unsigned long long)handles[i]);
    }
}

/* Program, sync, event                                                */

/* Host-only state for the offline SPIR-V cache.  The guest NVNprogram object
 * stays 0xC0 bytes; only this pointer is added to it. */
typedef struct {
    uint8_t *spv[RUNTIME_SHADER_STAGES];
    size_t spv_size[RUNTIME_SHADER_STAGES];
    char spv_path[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_PATH_MAX];
    char res_path[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_PATH_MAX];
    char key[RUNTIME_SHADER_STAGES][RUNTIME_SHADER_KEY_MAX + 1];
    RuntimeShaderIdentity id[RUNTIME_SHADER_STAGES];
    /* Bind-state variant (<key>-<suffix>.spv), loaded lazily at submit. */
    uint8_t *vspv[2];
    size_t vspv_size[2];
    char vspv_path[2][RUNTIME_SHADER_PATH_MAX];
    char vres_path[2][RUNTIME_SHADER_PATH_MAX];
    char variant[192];
    uint64_t variant_sig;
    unsigned loaded;                 /* bit per stage with a loaded module */
    GfxReflectedPipeline *pipeline;  /* lazy, keyed by (vs, ps, target layout) */
    int pipeline_format;
    uint64_t layout_sig;             /* render-target layout in the cache key */
    GfxRenderTargetLayout pipeline_layout;
    uint64_t vertex_sig;
} prog_shader_state_t;

static pthread_mutex_t g_prog_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_shader_cache_hits, g_shader_cache_misses, g_pipelines_created;

static void prog_stage_clear(prog_shader_state_t *s, unsigned stage) {
    if (!s || stage >= RUNTIME_SHADER_STAGES) return;
    free(s->spv[stage]);
    s->spv[stage] = NULL;
    s->spv_size[stage] = 0;
    s->spv_path[stage][0] = 0;
    s->res_path[stage][0] = 0;
    s->key[stage][0] = 0;
    memset(&s->id[stage], 0, sizeof s->id[stage]);
    if (stage < 2) {
        free(s->vspv[stage]);
        s->vspv[stage] = NULL;
        s->vspv_size[stage] = 0;
        s->vspv_path[stage][0] = 0;
        s->vres_path[stage][0] = 0;
        s->variant[0] = 0;
        s->variant_sig = 0;
    }
    s->loaded &= ~(1u << stage);
    if (s->pipeline) {
        gfx_backend_reflected_pipeline_destroy(s->pipeline);
        s->pipeline = NULL;
        s->pipeline_format = 0;
        s->layout_sig = 0;
    }
}

static void prog_state_clear_locked(prog_shader_state_t *s) {
    if (!s) return;
    for (unsigned i = 0; i < RUNTIME_SHADER_STAGES; i++) prog_stage_clear(s, i);
    s->loaded = 0;
}

/* Host-side program -> shader-state map.  NVNprogram is an opaque 0xC0-byte
 * guest object; keeping host pointers inside it lets guest writes clobber
 * them (TOTK overwrites the low half of the old inline shader_state field
 * with 0x0003 before the first draw).  The map lives outside guest memory. */
typedef struct prog_state_slot {
    struct prog_state_slot *next;
    prog_t *program;
    prog_shader_state_t *state;
} prog_state_slot_t;

#define PROG_STATE_BUCKETS 4096
static prog_state_slot_t *g_prog_states[PROG_STATE_BUCKETS];

static unsigned prog_state_hash(const prog_t *p) {
    uintptr_t v = (uintptr_t)p >> 4;
    v ^= v >> 17;
    v *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(v & (PROG_STATE_BUCKETS - 1));
}

static prog_shader_state_t *prog_state_find(prog_t *p) {
    for (prog_state_slot_t *e = g_prog_states[prog_state_hash(p)]; e; e = e->next)
        if (e->program == p) return e->state;
    return NULL;
}

static prog_shader_state_t *prog_state_install(prog_t *p, prog_shader_state_t *s) {
    unsigned b = prog_state_hash(p);
    for (prog_state_slot_t *e = g_prog_states[b]; e; e = e->next) {
        if (e->program == p) { e->state = s; return s; }
    }
    prog_state_slot_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->program = p;
    e->state = s;
    e->next = g_prog_states[b];
    g_prog_states[b] = e;
    return s;
}

static prog_shader_state_t *prog_state_remove(prog_t *p) {
    unsigned b = prog_state_hash(p);
    prog_state_slot_t **link = &g_prog_states[b];
    while (*link) {
        prog_state_slot_t *e = *link;
        if (e->program == p) {
            prog_shader_state_t *s = e->state;
            *link = e->next;
            free(e);
            return s;
        }
        link = &e->next;
    }
    return NULL;
}

static uint32_t nvnProgramInitialize(prog_t *p, const nvn_dev_t *d) { nvn_api("nvnProgramInitialize");
    if (!p || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnProgramInitialize(device)")) return 0;
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    p->serial = (uint64_t)(uintptr_t)p;
    return 1;
}
static void nvnProgramFinalize(prog_t *p) {
    if (!p) return;
    if (p->gfx_reflected_pipeline) {
        gfx_backend_reflected_pipeline_destroy((GfxReflectedPipeline *)p->gfx_reflected_pipeline);
        p->gfx_reflected_pipeline = NULL;
    }
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_remove(p);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] ProgramFinalize %p state=%p\n", (void *)p, (void *)s);
    if (s) {
        prog_state_clear_locked(s);
        free(s);
    }
    pthread_mutex_unlock(&g_prog_lock);
    p->magic = 0;
}
static void nvnProgramSetDebugLabel(prog_t *p, const char *l) { (void)p; (void)l; }

/* Optional offline capture: write the exact control/code pair the game binds
 * so a host-side totk_xlate run can produce its SPIR-V.  Game-derived bytes
 * stay in TOTK_ANDROID_SHADER_DUMP, normally under /data/local/tmp. */
static void nvn_dump_shader(const RuntimeShaderIdentity *id, const void *control,
                            const void *code) {
    const char *dir = getenv("TOTK_ANDROID_SHADER_DUMP");
    if (!dir || !*dir || !id || !control || !code) return;
    if (!id->control_key_bytes || !id->code_bytes) return;
    char path[RUNTIME_SHADER_PATH_MAX];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s.ctl", dir, id->key);
    if (stat(path, &st) == 0) return;
    mkdir(dir, 0755);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(control, 1, id->control_key_bytes, f) != id->control_key_bytes) {
        fclose(f); unlink(path); return;
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/%s.code", dir, id->key);
    f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(code, 1, id->code_bytes, f) != id->code_bytes) {
        fclose(f); unlink(path); return;
    }
    fclose(f);
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] dumped shader %s %s ctl=%u code=%u\n",
                runtime_shader_stage_name(id->stage), id->key,
                id->control_key_bytes, id->code_bytes);
}

static GfxReflectedPipeline *nvn_program_cached_pipeline(prog_t *p, uint32_t nvn_format,
                                                         const GfxRenderTargetLayout *layout,
                                                         int legacy_window_target,
                                                         const GfxVertexLayout *input,
                                                         uint64_t vertex_sig,
                                                         const char *variant);
static uint32_t nvn_program_set_shaders(prog_t *p, int n, const void *shaders,
                                        size_t stride, const char *what) {
    if (!nvn_obj(p, NVN_MAGIC('P', 'R', 'O', 'G'), what)) return 0;
    if (n < 0 || n > 6 || (!shaders && n)) { nvn_fail(what, "invalid stage count"); return 0; }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] %s entry p=%p n=%d stride=%zu thread=%#x\n",
                what, (void *)p, n, stride, runtime_thread_current_handle());
    p->nstage = (uint32_t)n;

    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (s && !prog_state_install(p, s)) { free(s); s = NULL; }
    }
    if (!s) {
        pthread_mutex_unlock(&g_prog_lock);
        nvn_fail(what, "out of memory for shader state");
        return 0;
    }
    /* A full multi-stage call is a new program state.  A single-stage call
     * replaces just that stage (some engines set VS and PS separately). */
    if (n > 1) prog_state_clear_locked(s);

    for (int i = 0; i < n; i++) {
        const uint8_t *entry = (const uint8_t *)shaders + (size_t)i * stride;
        uint64_t data = 0;
        const void *control = NULL;
        memcpy(&data, entry, 8);
        memcpy(&control, entry + 8, 8);
        p->data[i] = data;
        p->control[i] = control;

        RuntimeShaderIdentity id;
        const void *code = NULL;
        RuntimeShaderResolveInfo resolved;
        int have_code = runtime_shader_resolve_code(control, data, &code, &resolved);
        if (!have_code && (data < 0x100000 || nvn_trace())) {
            fprintf(stderr, "[totk-nvn] %s unresolved low data: n=%d stride=%zu slot=%d"
                            " control=%p data=%#llx region=%#lx-%#lx pkg=%#lx"
                            " pool=%#x+%#x candidate=%#lx why=%s\n",
                    what, n, stride, i, control, (unsigned long long)data,
                    (unsigned long)resolved.region_start,
                    (unsigned long)resolved.region_end,
                    (unsigned long)resolved.package_base,
                    resolved.pool_offset, resolved.pool_size,
                    (unsigned long)resolved.candidate,
                    resolved.why ? resolved.why : "?");
        }
        if (!have_code || !runtime_shader_identity(control, code, &id)) {
            fprintf(stderr, "[totk-nvn] %s program=%p slot=%d: no NVN shader identity"
                            " (control=%p data=%#llx code=%p why=%s)\n",
                    what, (void *)p, i, control, (unsigned long long)data, code,
                    resolved.why ? resolved.why : "?");
            continue;
        }
        if (data < 0x100000 && (nvn_trace() || i == 0))
            fprintf(stderr, "[totk-nvn] %s resolved low data slot=%d %#llx -> %p"
                            " (%s pkg=%#lx pool=%#x+%#x bytes=%u)\n",
                    what, i, (unsigned long long)data, code,
                    resolved.why ? resolved.why : "?", (unsigned long)resolved.package_base,
                    resolved.pool_offset, resolved.pool_size, id.code_bytes);
        RuntimeShaderModule mod;
        if (runtime_shader_cache_load(runtime_shader_cache_root(), &id, &mod)) {
            prog_stage_clear(s, id.stage);
            s->spv[id.stage] = mod.spv;
            s->spv_size[id.stage] = mod.spv_size;
            memcpy(s->spv_path[id.stage], mod.spv_path, sizeof mod.spv_path);
            memcpy(s->res_path[id.stage], mod.res_path, sizeof mod.res_path);
            memcpy(s->key[id.stage], id.key, sizeof id.key);
            s->id[id.stage] = id;
            s->loaded |= 1u << id.stage;
            unsigned hit = __atomic_add_fetch(&g_shader_cache_hits, 1, __ATOMIC_RELAXED);
            if (hit <= 16 || nvn_trace())
                fprintf(stderr, "[totk-nvn] shader cache hit #%u %s %s code=%u%s\n",
                        hit, runtime_shader_stage_name(id.stage), id.key, id.code_bytes,
                        mod.res_path[0] ? " +res.json" : "");
        } else {
            nvn_dump_shader(&id, control, code);
            prog_stage_clear(s, id.stage);
            unsigned miss = __atomic_add_fetch(&g_shader_cache_misses, 1, __ATOMIC_RELAXED);
            if (miss <= 32 || nvn_trace())
                fprintf(stderr, "[totk-nvn] shader cache MISS #%u %s %s code=%u root=%s\n",
                        miss, runtime_shader_stage_name(id.stage), id.key, id.code_bytes,
                        runtime_shader_cache_root());
        }
    }

    int ready = (s->loaded & ((1u << 0) | (1u << 1))) == ((1u << 0) | (1u << 1));
    if (ready && nvn_trace())
        fprintf(stderr, "[totk-nvn] program=%p has cached vs=%s ps=%s state=%p loaded=%#x\n",
                (void *)p, s->key[0], s->key[1], (void *)s, s->loaded);
    pthread_mutex_unlock(&g_prog_lock);
    if (ready) {
        /* Prime the reflected pipeline on the game/recording thread.  A first
         * vkCreateGraphicsPipelines issued on the submit thread for these real
         * TOTK shaders aborts inside the driver; the same call succeeds here
         * and the submit path then reuses the cached pipeline (same layout,
         * format and window signature).  Only the first ready program warms
         * the driver; later pipelines reuse or build normally. */
        static int warmed;
        if (!warmed) {
            warmed = 1;
            GfxRenderTargetLayout warm_layout = { 0 };
            warm_layout.color_count = 1;
            warm_layout.color_formats[0] = gfx_backend_vk_format_for_nvn(0x25);
            GfxReflectedPipeline *rp =
                nvn_program_cached_pipeline(p, 0x25, &warm_layout, 1, NULL, 0, "");
            if (nvn_trace())
                fprintf(stderr, "[totk-nvn] warm pipeline program=%p -> %p\n",
                        (void *)p, (void *)rp);
        }
    }
    return 1;
}

static uint32_t nvnProgramSetShaders(prog_t *p, int n, const void *shaders) {
    return nvn_program_set_shaders(p, n, shaders, 16, "nvnProgramSetShaders");
}
static uint32_t nvnProgramSetShadersExt(prog_t *p, int n, const void *shaders) {
    /* TOTK's NVNshaderDataExt is 0x18 bytes: {data, control, reserved}. */
    return nvn_program_set_shaders(p, n, shaders, 0x18, "nvnProgramSetShadersExt");
}
static int nvn_read_file(const char *path, uint8_t **out, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n <= 0 || n > (16 << 20)) { fclose(f); return 0; }
    rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (!b) { fclose(f); return 0; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = b; *size = (size_t)n;
    return 1;
}

/* Host bridge for the 2B.2/2B.3 integration: load translated SPIR-V into an
 * NVN program object.  This is not a guest NVN entry point; the guest still
 * carries Maxwell data/control, and the offline cache supplies SPIR-V. */
int runtime_nvn_program_load_spirv(void *prog, const char *vs_path, const char *ps_path) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') || !vs_path || !ps_path) return 0;
    uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    if (!nvn_read_file(vs_path, &vs, &vs_size) || !nvn_read_file(ps_path, &ps, &ps_size)) {
        free(vs); free(ps);
        fprintf(stderr, "[totk-nvn] ERROR read SPIR-V %s / %s\n", vs_path, ps_path);
        return 0;
    }
    int fmt = gfx_backend_vk_format_for_nvn(0x25);
    GfxTrianglePipeline *pipeline = gfx_backend_triangle_pipeline_create(vs, vs_size, ps, ps_size, fmt);
    free(vs); free(ps);
    if (!pipeline) {
        fprintf(stderr, "[totk-nvn] ERROR create pipeline from translated SPIR-V\n");
        return 0;
    }
    p->gfx_pipeline = pipeline;
    fprintf(stderr, "[totk-nvn] loaded translated SPIR-V vs=%s ps=%s\n", vs_path, ps_path);
    return 1;
}

/* Load a translated pair with the complete .res.json descriptor map.  The
 * vertex side stays in set 0 and the pixel side is moved to set 1, matching
 * the Android reflected-pipeline convention. */
int runtime_nvn_program_load_spirv_reflect(void *prog, const char *vs_path, const char *ps_path,
                                           const char *vs_res, const char *ps_res) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') ||
        !vs_path || !ps_path || !vs_res || !ps_res) return 0;
    uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    if (!nvn_read_file(vs_path, &vs, &vs_size) || !nvn_read_file(ps_path, &ps, &ps_size)) {
        free(vs); free(ps);
        fprintf(stderr, "[totk-nvn] ERROR read SPIR-V %s / %s\n", vs_path, ps_path);
        return 0;
    }
    gfx_spirv_move_set(ps, ps_size, 0, 1);
    GfxBindingSpec vss[64], pss[64];
    unsigned nvs = gfx_reflect_full(vs_res, vs_path, 0, vss, 64);
    unsigned nps = gfx_reflect_full(ps_res, ps_path, 1, pss, 64);
    int fmt = gfx_backend_vk_format_for_nvn(0x25);
    GfxReflectedPipeline *pipeline = gfx_backend_reflected_pipeline_create(
        vs, vs_size, ps, ps_size, fmt, vss, nvs, pss, nps);
    free(vs); free(ps);
    if (!pipeline) {
        fprintf(stderr, "[totk-nvn] ERROR create reflected pipeline vs=%s ps=%s\n", vs_path, ps_path);
        return 0;
    }
    p->gfx_reflected_pipeline = pipeline;
    fprintf(stderr, "[totk-nvn] loaded reflected SPIR-V vs=%s(%u) ps=%s(%u)\n",
            vs_path, nvs, ps_path, nps);
    return 1;
}

int runtime_nvn_program_bind_texture(void *prog, uint32_t set, uint32_t binding,
                                     const void *texture) {
    prog_t *p = prog;
    const tex_t *t = texture;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') || !p->gfx_reflected_pipeline ||
        !t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return 0;
    GfxTexture *g = nvn_texture_gfx(t);
    if (!g) return 0;
    return gfx_backend_reflected_bind_texture((GfxReflectedPipeline *)p->gfx_reflected_pipeline,
                                              set, binding, g);
}

int runtime_nvn_program_write_uniform(void *prog, uint32_t set, uint32_t binding,
                                      uint32_t offset, const void *data, size_t size) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') || !p->gfx_reflected_pipeline)
        return 0;
    return gfx_backend_reflected_write_uniform((GfxReflectedPipeline *)p->gfx_reflected_pipeline,
                                               set, binding, offset, data, size);
}

int runtime_nvn_program_write_storage(void *prog, uint32_t set, uint32_t binding,
                                      uint32_t offset, const void *data, size_t size) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') || !p->gfx_reflected_pipeline)
        return 0;
    return gfx_backend_reflected_write_storage((GfxReflectedPipeline *)p->gfx_reflected_pipeline,
                                               set, binding, offset, data, size);
}

static void nvnProgramSetSampleShading(prog_t *p, int v) { (void)p; (void)v; }
static int nvnProgramGetSubtileSize(const prog_t *p) { (void)p; return 0x1000; }
static void nvnProgramSetSubroutineLinkage(prog_t *p, int n, const void *l) { (void)p; (void)n; (void)l; }

static uint32_t nvn_window_format(const win_t *w) {
    uint32_t format = 0;
    if (w && runtime_native_window_valid(w->native)) {
        if (runtime_native_window_info(w->native, NULL, NULL, &format, NULL, NULL) && format)
            return format;
    }
    return 0x25;
}

/* Create (or reuse) the host pipeline for a program whose VS/PS came from the
 * offline cache.  The target format is part of the identity: the same program
 * can be drawn to the handheld and docked windows. */
static char nvn_attr_class(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17:
    case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F:
        return 'u';
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18:
    case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30:
        return 's';
    default:
        return 'f';
    }
}

static uint64_t nvn_variant_sig(const char *v) {
    if (!v || !*v) return 0;
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        h ^= *p; h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static const char *nvn_tex_target_name(int target) {
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

/* Bind-state suffix shared with the offline packer:
 *   a<16 chars: f/u/s/_>  [-t<slot>_<target>[d]]...
 * Only emitted when the cache actually contains <key>-*.spv.  The offline
 * translator must be run with the matching attr/tex answers. */
static void nvn_variant_suffix(prog_t *p, const cb_t *cb, char *out, size_t cap) {
    out[0] = 0;
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = p ? prog_state_find(p) : NULL;
    pthread_mutex_unlock(&g_prog_lock);
    if (!s || !cb || !cb->bind_state) return;
    if (!runtime_shader_cache_has_variants(runtime_shader_cache_root(), &s->id[0]) &&
        !runtime_shader_cache_has_variants(runtime_shader_cache_root(), &s->id[1]))
        return;
    const nvn_bind_state_t *b = cb->bind_state;
    size_t pos = 0;
    int any = 0;
    char attr[18];
    attr[0] = 'a';
    for (int i = 0; i < 16; i++) {
        char c = '_';
        if ((b->va_mask & (1u << i)) && (b->va[i] & 0xffu))
            c = nvn_attr_class(b->va[i] & 0xffu);
        attr[1 + i] = c;
        if (c != '_') any = 1;
    }
    attr[17] = 0;
    if (any && pos + 17 < cap) {
        memcpy(out + pos, attr, 17);
        pos += 17;
        out[pos] = 0;
    }
    GfxBindingSpec pss[64];
    unsigned nps = gfx_reflect_full(s->res_path[1][0] ? s->res_path[1] : NULL,
                                    s->spv_path[1], 1, pss, 64);
    uint32_t seen_slots[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < nps; i++) {
        const GfxBindingSpec *sp = &pss[i];
        if (sp->kind != GFX_BIND_COMBINED_IMAGE_SAMPLER &&
            sp->kind != GFX_BIND_SAMPLED_IMAGE) continue;
        if (sp->nvn_slot < 0 || sp->nvn_slot >= 128) continue;
        int sbit = sp->nvn_slot;
        if (seen_slots[sbit / 32] & (1u << (sbit % 32))) continue;
        uint64_t handle = b->tex[1][sp->nvn_slot];
        if (!handle) continue;
        int tid = (int)(handle & 0xfffff);
        const tex_t *t = cb->tex_pool ? nvn_tex_pool_lookup(cb->tex_pool, tid) : NULL;
        if (!t) continue;
        seen_slots[sbit / 32] |= 1u << (sbit % 32);
        const char *tn = nvn_tex_target_name((int)t->d.target);
        int depth = t->d.format >= 0x32 && t->d.format <= 0x36;
        int n = snprintf(out + pos, cap - pos, "%st%d_%s%s", pos ? "-" : "",
                         sp->nvn_slot, tn, depth ? "d" : "");
        if (n < 0 || (size_t)n >= cap - pos) { out[0] = 0; return; }
        pos += (size_t)n;
    }
}

static GfxReflectedPipeline *nvn_program_cached_pipeline(prog_t *p, uint32_t nvn_format,
                                                         const GfxRenderTargetLayout *layout,
                                                         int legacy_window_target,
                                                         const GfxVertexLayout *input,
                                                         uint64_t vertex_sig,
                                                         const char *variant) {
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (!s || (s->loaded & ((1u << 0) | (1u << 1))) != ((1u << 0) | (1u << 1))) {
        if (nvn_trace())
            fprintf(stderr, "[totk-nvn] cached pipeline: program=%p state=%p loaded=%#x (stages missing)\n",
                    (void *)p, (void *)s, s ? s->loaded : 0);
        pthread_mutex_unlock(&g_prog_lock);
        return NULL;
    }
    int fmt = (layout && layout->color_count) ? layout->color_formats[0] : 0;
    if (!fmt) fmt = gfx_backend_vk_format_for_nvn(nvn_format);
    if (!fmt) {
        if (nvn_trace())
            fprintf(stderr, "[totk-nvn] cached pipeline: program=%p no format"
                            " layout=%p colors=%u layout_fmt=%#x nvn_fmt=%#x legacy=%d\n",
                    (void *)p, (const void *)layout, layout ? layout->color_count : 0,
                    layout ? layout->color_formats[0] : 0, nvn_format, legacy_window_target);
        pthread_mutex_unlock(&g_prog_lock);
        return NULL;
    }
    uint64_t variant_sig = nvn_variant_sig(variant);
    uint64_t layout_sig = nvn_layout_sig(layout);
    if (legacy_window_target) layout_sig ^= 0x9e3779b97f4a7c15ull;
    if (s->pipeline && s->pipeline_format == fmt && s->vertex_sig == vertex_sig &&
        s->variant_sig == variant_sig && s->layout_sig == layout_sig) {
        GfxReflectedPipeline *ready = s->pipeline;
        pthread_mutex_unlock(&g_prog_lock);
        return ready;
    }
    if (s->pipeline && (s->variant_sig != variant_sig || s->layout_sig != layout_sig)) {
        gfx_backend_reflected_pipeline_destroy(s->pipeline);
        s->pipeline = NULL;
    }
    if (strcmp(s->variant, variant ? variant : "")) {
        for (unsigned st = 0; st < 2; st++) {
            free(s->vspv[st]);
            s->vspv[st] = NULL;
            s->vspv_size[st] = 0;
            s->vspv_path[st][0] = 0;
            s->vres_path[st][0] = 0;
            if (!variant || !*variant || !(s->loaded & (1u << st))) continue;
            RuntimeShaderModule vm;
            if (!runtime_shader_cache_load_variant(runtime_shader_cache_root(), &s->id[st],
                                                   variant, &vm)) continue;
            if (strcmp(vm.spv_path, s->spv_path[st]) != 0) {
                s->vspv[st] = vm.spv;
                s->vspv_size[st] = vm.spv_size;
                memcpy(s->vspv_path[st], vm.spv_path, sizeof s->vspv_path[st]);
                memcpy(s->vres_path[st], vm.res_path, sizeof s->vres_path[st]);
            } else {
                runtime_shader_cache_module_free(&vm);
            }
        }
        snprintf(s->variant, sizeof s->variant, "%s", variant ? variant : "");
    }
    if (s->pipeline) {
        gfx_backend_reflected_pipeline_destroy(s->pipeline);
        s->pipeline = NULL;
    }
    const uint8_t *uspv[2] = { s->vspv[0] ? s->vspv[0] : s->spv[0],
                               s->vspv[1] ? s->vspv[1] : s->spv[1] };
    size_t usize[2] = { s->vspv[0] ? s->vspv_size[0] : s->spv_size[0],
                        s->vspv[1] ? s->vspv_size[1] : s->spv_size[1] };
    /* The offline PS descriptor set is 0 (the translator puts every resource
     * in one set); the reflected pipeline layout keeps VS in set 0 and PS in
     * set 1.  Move the PS decorations in place before the pipeline is built,
     * exactly as runtime_nvn_program_load_spirv_reflect() does.  Without this,
     * vkCreateGraphicsPipelines sees a shader that disagrees with its
     * descriptor set layout. */
    gfx_spirv_move_set((uint8_t *)uspv[1], usize[1], 0, 1);
    const char *ures[2] = { s->vres_path[0][0] ? s->vres_path[0] : s->res_path[0],
                            s->vres_path[1][0] ? s->vres_path[1] : s->res_path[1] };
    const char *upath[2] = { s->vspv[0] ? s->vspv_path[0] : s->spv_path[0],
                             s->vspv[1] ? s->vspv_path[1] : s->spv_path[1] };
    GfxBindingSpec vss[64], pss[64];
    unsigned nvs = gfx_reflect_full(ures[0][0] ? ures[0] : NULL, upath[0], 0, vss, 64);
    unsigned nps = gfx_reflect_full(ures[1][0] ? ures[1] : NULL, upath[1], 1, pss, 64);
    unsigned created = __atomic_add_fetch(&g_pipelines_created, 1, __ATOMIC_RELAXED);
    if (created <= 32 || nvn_trace())
        fprintf(stderr, "[totk-nvn] program=%p create pipeline from cache #%u vs=%s(%u) ps=%s(%u)"
                        " format=%#x colors=%u depth=%#x attrs=%u streams=%u variant=%s\n",
                (void *)p, created, s->key[0], nvs, s->key[1], nps, nvn_format,
                layout ? layout->color_count : 0, layout ? layout->depth_format : 0,
                input ? input->nattributes : 0, input ? input->nbindings : 0,
                variant && *variant ? variant : "-");
    /* The legacy window/single-target image (gfx_backend_offscreen_create) is
     * color-only from NVN's point of view; keep it on the old create_ex path so
     * that path keeps working, and use the layout-aware variant whenever NVN
     * asked for MRT or depth.  (L3 note: as of the merge test the legacy
     * target reports depth=VK_FORMAT_D24_UNORM_S8_UINT, so a create_layout
     * pipeline with depth=0 is rejected by the backend's layout check.) */
    int use_layout = layout && !legacy_window_target &&
                     (layout->color_count > 1 || layout->depth_format != 0);
    s->pipeline = use_layout
        ? gfx_backend_reflected_pipeline_create_layout(uspv[0], usize[0], uspv[1], usize[1],
                                                       layout, vss, nvs, pss, nps, input)
        : gfx_backend_reflected_pipeline_create_ex(uspv[0], usize[0], uspv[1], usize[1],
                                                   fmt, vss, nvs, pss, nps, input);
    if (s->pipeline) {
        s->pipeline_format = fmt;
        s->vertex_sig = vertex_sig;
        s->variant_sig = variant_sig;
        s->layout_sig = layout_sig;
        if (layout) s->pipeline_layout = *layout;
        else memset(&s->pipeline_layout, 0, sizeof s->pipeline_layout);
    }
    GfxReflectedPipeline *ready = s->pipeline;
    pthread_mutex_unlock(&g_prog_lock);
    return ready;
}

static uint64_t nvn_vertex_signature(const GfxVertexLayout *layout) {
    if (!layout || !layout->nattributes) return 0;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < layout->nattributes; i++) {
        const GfxVertexAttribute *a = &layout->attributes[i];
        uint64_t v[4] = { a->location, a->binding, (uint64_t)(uint32_t)a->format, a->offset };
        for (unsigned k = 0; k < 4; k++) { h ^= v[k]; h *= 1099511628211ull; }
    }
    for (uint32_t i = 0; i < layout->nbindings; i++) {
        uint64_t v[3] = { layout->bindings[i].binding, layout->bindings[i].stride,
                          layout->bindings[i].input_rate };
        for (unsigned k = 0; k < 3; k++) { h ^= v[k]; h *= 1099511628211ull; }
    }
    return h ? h : 1;
}

/* Decode the recorded NVN attribute/stream state.  `va_st` is packed as
 * {fmt:8, offset:19, stream:5}; `vs_st` is {stride, divisor}. */
static void nvn_build_vertex_layout(const cb_t *cb, GfxVertexLayout *out, uint64_t *sig) {
    memset(out, 0, sizeof *out);
    *sig = 0;
    const nvn_bind_state_t *b = cb ? cb->bind_state : NULL;
    if (!b || !b->va_mask) return;
    uint32_t used_streams = 0;
    uint32_t valid_streams = 0;
    for (int i = 0; i < 16; i++) {
        if (!(b->va_mask & (1u << i))) continue;
        uint32_t va = b->va[i];
        uint32_t fmt = va & 0xffu;
        uint32_t off = (va >> 8) & 0x7ffffu;
        uint32_t stream = (va >> 27) & 0x1fu;
        if (!fmt) continue;
        if (stream >= 16) {
            nvn_fail("nvnQueueSubmitCommands", "vertex attribute stream out of range");
            continue;
        }
        int vkfmt = gfx_backend_vk_format_for_nvn(fmt);
        if (!vkfmt) {
            static unsigned bad_logged;
            if (bad_logged++ < 16)
                fprintf(stderr, "[totk-nvn] unsupported NVN vertex format %#x at location %d\n", fmt, i);
            continue;
        }
        GfxVertexAttribute *a = &out->attributes[out->nattributes++];
        a->location = (uint32_t)i;
        a->binding = stream;
        a->format = vkfmt;
        a->offset = off;
        used_streams |= 1u << stream;
    }
    for (int i = 0; i < 16; i++) {
        if (!(used_streams & (1u << i))) continue;
        uint32_t stride = (uint32_t)b->vs[i];
        uint32_t divisor = (uint32_t)(b->vs[i] >> 32);
        if (!(b->vs_mask & (1u << i)) || !stride) {
            static unsigned bad_stride;
            if (bad_stride++ < 16)
                fprintf(stderr, "[totk-nvn] vertex stream %d has no stride (attrs use it)\n", i);
            continue;
        }
        GfxVertexBinding *v = &out->bindings[out->nbindings++];
        v->binding = (uint32_t)i;
        v->stride = stride;
        v->input_rate = divisor ? 1 : 0;
        valid_streams |= 1u << i;
    }
    /* An attribute whose stream has no binding would make the pipeline
     * invalid; drop it instead of referencing a missing binding. */
    if (valid_streams != used_streams) {
        uint32_t n = 0;
        for (uint32_t k = 0; k < out->nattributes; k++)
            if (valid_streams & (1u << out->attributes[k].binding))
                out->attributes[n++] = out->attributes[k];
        out->nattributes = n;
    }
    *sig = nvn_vertex_signature(out);
}

static uint32_t nvn_build_vertex_streams(const cb_t *cb, GfxVertexStream *out, uint32_t max) {
    const nvn_bind_state_t *b = cb ? cb->bind_state : NULL;
    if (!b) return 0;
    uint32_t n = 0;
    for (int i = 0; i < 16 && n < max; i++) {
        if (!(b->vb_mask & (1u << i))) continue;
        if (!b->vb[i].address || !b->vb[i].size) continue;
        size_t size = b->vb[i].size;
        if (size > (16u << 20)) size = 16u << 20;
        out[n].binding = (uint32_t)i;
        out[n].data = (const void *)(uintptr_t)b->vb[i].address;
        out[n].size = size;
        n++;
    }
    return n;
}

/* Apply the last UBO/SSBO/texture state recorded by the command buffer.  This
 * is a per-submit state model; NVN games normally bind then draw in order. */
static void nvn_apply_bind_state(prog_t *p, GfxReflectedPipeline *rp, cb_t *cb) {
    (void)p;
    const nvn_bind_state_t *b = cb ? cb->bind_state : NULL;
    if (!b || !rp) return;
    for (int stage = 0; stage < 2; stage++) {
        for (int i = 0; i < 16; i++) {
            if (b->ubo_mask[stage] & (1u << i)) {
                const void *data = (const void *)(uintptr_t)b->ubo[stage][i].address;
                if (!gfx_backend_reflected_bind_ubo_slot(rp, (uint32_t)stage, (uint32_t)i,
                                                         data, b->ubo[stage][i].size) &&
                    nvn_trace())
                    fprintf(stderr, "[totk-nvn] UBO stage=%d slot=%d size=%zu not in reflection\n",
                            stage, i, b->ubo[stage][i].size);
            }
            if (b->ssbo_mask[stage] & (1u << i)) {
                const void *data = (const void *)(uintptr_t)b->ssbo[stage][i].address;
                if (!gfx_backend_reflected_bind_storage_slot(rp, (uint32_t)stage, (uint32_t)i,
                                                             data, b->ssbo[stage][i].size) &&
                    nvn_trace())
                    fprintf(stderr, "[totk-nvn] SSBO stage=%d slot=%d size=%zu not in reflection\n",
                            stage, i, b->ssbo[stage][i].size);
            }
        }
        for (int i = 0; i < 128; i++) {
            if (!(b->tex_mask[stage][i / 32] & (1u << (i % 32)))) continue;
            uint64_t handle = b->tex[stage][i];
            int tid = (int)(handle & 0xfffff);
            const tex_t *t = cb->tex_pool ? nvn_tex_pool_lookup(cb->tex_pool, tid) : NULL;
            if (!t) continue;
            GfxTexture *g = nvn_texture_gfx_ready(t);   /* lazy first-bind upload */
            if (g && !gfx_backend_reflected_bind_texture_slot(rp, (uint32_t)stage, (uint32_t)i, g) &&
                nvn_trace())
                fprintf(stderr, "[totk-nvn] texture stage=%d slot=%d handle=%#llx not in reflection\n",
                        stage, i, (unsigned long long)handle);
        }
    }
}

/* Translate the recorded NVN command order to the backend's clear/viewport/
 * scissor/draw list.  Resource binds stay in the reflected descriptor sets. */
static void nvn_state_to_gfx(const nvn_render_state_t *rs, GfxPipelineState *o) {
    memset(o, 0, sizeof *o);
    o->blend_enable = rs->color.blend_en & 1;
    o->blend_src_rgb = rs->blend.src_rgb;
    o->blend_dst_rgb = rs->blend.dst_rgb;
    o->blend_src_a = rs->blend.src_a;
    o->blend_dst_a = rs->blend.dst_a;
    o->blend_eq_rgb = rs->blend.eq_rgb;
    o->blend_eq_a = rs->blend.eq_a;
    o->color_write_mask = rs->chmask.mask & 0xf;
    o->depth_test = rs->ds.dt;
    o->depth_write = rs->ds.dw;
    o->depth_func = rs->ds.df;
    o->stencil_test = rs->ds.st;
    o->stencil_func_front = rs->ds.sf_f;
    o->stencil_func_back = rs->ds.sf_b;
    o->stencil_fail_front = rs->ds.fail_f;
    o->stencil_zfail_front = rs->ds.zfail_f;
    o->stencil_pass_front = rs->ds.zpass_f;
    o->stencil_fail_back = rs->ds.fail_b;
    o->stencil_zfail_back = rs->ds.zfail_b;
    o->stencil_pass_back = rs->ds.zpass_b;
    o->cull_mode = rs->poly.cull;                  /* VkCullModeFlags values match */
    o->front_face = rs->poly.front ? 0 : 1;        /* CCW=0, CW=1 in Vulkan */
    o->polygon_mode = rs->poly.mode == 2 ? 0 : rs->poly.mode == 1 ? 1 : 2;
    o->stencil_ref = (uint32_t)rs->stencil_ref;
    memcpy(o->blend_color, rs->blend_color, sizeof o->blend_color);
}

static uint32_t nvn_translate_commands(const cb_t *cb, GfxCmd *out, uint32_t max,
                                       GfxOffscreenTarget *target,
                                       float clear[4], int *has_clear) {
    uint32_t n = 0;
    *has_clear = 0;
    if (!cb) return 0;
    nvn_render_state_t defaults;
    nvn_render_state_defaults(&defaults);
    GfxPipelineState cur_state;
    nvn_state_to_gfx(&defaults, &cur_state);
    for (uint32_t i = 0; i < cb->ncmds && n < max; i++) {
        const nvn_cmd_t *c = &cb->cmds[i];
        switch (c->kind) {
        case NVN_CMD_CLEAR_COLOR:
            if (c->u.clear.index != 0) {
                /* GfxCmd has one clear colour for the pass: the contract has no
                 * per-attachment index, so every attachment is cleared with
                 * attachment 0's value.  Different per-target colours need an
                 * additive contract field (reported to the lead). */
                static unsigned logged;
                if (logged++ < 8)
                    fprintf(stderr, "[totk-nvn] ClearColor target=%d folded into the pass clear"
                                    " (GfxCmd.clear has no attachment index)\n", c->u.clear.index);
                break;
            }
            memcpy(clear, c->u.clear.rgba, 4 * sizeof(float));
            *has_clear = 1;
            out[n].kind = GFX_CMD_CLEAR_COLOR;
            memcpy(out[n].u.clear.rgba, c->u.clear.rgba, 4 * sizeof(float));
            out[n].u.clear.mask = c->u.clear.mask;
            n++;
            break;
        case NVN_CMD_SET_VIEWPORT:
            out[n].kind = GFX_CMD_SET_VIEWPORT;
            out[n].u.rect.x = c->u.rect.x;
            out[n].u.rect.y = c->u.rect.y;
            out[n].u.rect.width = c->u.rect.width;
            out[n].u.rect.height = c->u.rect.height;
            n++;
            break;
        case NVN_CMD_SET_SCISSOR:
            out[n].kind = GFX_CMD_SET_SCISSOR;
            out[n].u.rect.x = c->u.rect.x;
            out[n].u.rect.y = c->u.rect.y;
            out[n].u.rect.width = c->u.rect.width;
            out[n].u.rect.height = c->u.rect.height;
            n++;
            break;
        case NVN_CMD_BIND_STATE:
            nvn_state_to_gfx(&c->u.state.rs, &cur_state);
            break;
        case NVN_CMD_CLEAR_DEPTH_STENCIL:
            out[n].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
            out[n].u.clear_depth.depth = c->u.clear_ds.depth;
            out[n].u.clear_depth.dmask = c->u.clear_ds.dmask;
            out[n].u.clear_depth.stencil = c->u.clear_ds.stencil;
            out[n].u.clear_depth.smask = c->u.clear_ds.smask;
            n++;
            break;
        case NVN_CMD_BIND_DESC: {
            out[n].kind = GFX_CMD_BIND_DESC;
            out[n].u.bind.stage = c->u.desc.stage;
            out[n].u.bind.kind = c->u.desc.kind;
            out[n].u.bind.slot = c->u.desc.slot;
            if (c->u.desc.kind == 2) {
                uint64_t h = c->u.desc.handle;
                int tid = (int)(h & 0xfffff);
                int sid = (int)((h >> 20) & 0xfff);
                const tex_t *t = (h && tid != 0xfffff && cb->tex_pool)
                    ? nvn_tex_pool_lookup(cb->tex_pool, tid) : NULL;
                const samp_t *sp = (h && cb->samp_pool)
                    ? nvn_samp_pool_lookup(cb->samp_pool, sid) : NULL;
                out[n].u.bind.texture = t ? nvn_texture_gfx_ready(t) : NULL;
                out[n].u.bind.sampler = sp ? nvn_sampler_gfx(sp) : NULL;
            } else {
                out[n].u.bind.data = (const void *)(uintptr_t)c->u.desc.address;
                out[n].u.bind.size = c->u.desc.size;
            }
            n++;
            break;
        }
        case NVN_CMD_DRAW:
            if (c->u.draw.instances > 1)
                nvn_fail("nvnQueueSubmitCommands", "instanced draw uses one instance for now");
            out[n].kind = GFX_CMD_DRAW;
            out[n].u.draw.primitive = c->u.draw.primitive;
            out[n].u.draw.first = c->u.draw.first;
            out[n].u.draw.count = c->u.draw.count;
            out[n].u.draw.state = cur_state;
            out[n].u.draw.has_state = 1;
            n++;
            break;
        case NVN_CMD_DRAW_INDEXED:
            out[n].kind = GFX_CMD_DRAW_INDEXED;
            out[n].u.draw_indexed.primitive = c->u.draw_indexed.primitive;
            out[n].u.draw_indexed.count = c->u.draw_indexed.count;
            out[n].u.draw_indexed.index_type = c->u.draw_indexed.index_type;
            out[n].u.draw_indexed.base_vertex = c->u.draw_indexed.base_vertex;
            out[n].u.draw_indexed.indices = (const void *)(uintptr_t)c->u.draw_indexed.indices;
            out[n].u.draw_indexed.index_size = (size_t)c->u.draw_indexed.count *
                (c->u.draw_indexed.index_type == 2 ? 4 :
                 c->u.draw_indexed.index_type == 1 ? 2 : 1);
            out[n].u.draw_indexed.state = cur_state;
            out[n].u.draw_indexed.has_state = 1;
            n++;
            break;
        case NVN_CMD_SET_RENDER_TARGETS:
            /* The host target is already the execute argument; emit the
             * explicit command when this pass has its own MRT image so the
             * backend binds exactly the resolved layout. */
            if (target) {
                out[n].kind = GFX_CMD_SET_RENDER_TARGETS;
                out[n].u.set_targets.target = target;
                n++;
            }
            break;
        default:
            break;
        }
    }
    return n;
}

/* True when the command buffer's target set is exactly the window's single
 * color target: draws and present then share the swapchain image instead of a
 * separate MRT image. */
static int nvn_layout_matches_window(const win_t *w, const GfxRenderTargetLayout *layout,
                                     uint32_t width, uint32_t height) {
    if (!layout || layout->color_count != 1 || layout->depth_format) return 0;
    GfxRenderTargetLayout wl;
    uint32_t ww = 0, wh = 0;
    nvn_window_layout(w, &wl, &ww, &wh);
    return ww == width && wh == height && wl.color_formats[0] == layout->color_formats[0];
}

/* Copy color attachment 0 of a host MRT image into the window's CPU mirror so
 * the existing NativeWindow readback/present path keeps observing the frame. */
static int nvn_window_mirror_target(win_t *w, GfxOffscreenTarget *target) {
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

static void nvn_process_submit_commands(const uint64_t *cmds, int n) {
    for (int i = 0; i < n; i++) {
        cb_t *cb = NULL;
        for (unsigned j = 0; j < g_ncbs; j++) {
            cb_t *cand = g_cbs[j];
            if (!cand || cand->magic != NVN_MAGIC('C', 'M', 'D', 'B')) continue;
            if (cand->handle == cmds[i]) { cb = cand; break; }
        }
        if (!cb || (!cb->draw_count && !cb->ncmds)) continue;
        prog_t *p = cb->prog;
        win_t *w = cb->win;
        int has_program = p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G');
        if (!w || w->magic != NVN_MAGIC('W', 'I', 'N', 'D') ||
            !runtime_native_window_valid(w->native)) {
            nvn_fail("nvnQueueSubmitCommands", "command buffer has no drawable window");
            continue;
        }

        /* Resolve the pass target layout.  nvnCommandBufferSetRenderTargets
         * records it; a command buffer that only set a window uses that
         * window's single-target layout. */
        GfxRenderTargetLayout layout;
        uint32_t rw = 0, rh = 0;
        int have_layout = 0;
        nvn_cb_target_t *cbt = nvn_cb_target(cb, 0);
        if (cbt && cbt->valid) {
            layout = cbt->layout;
            rw = cbt->width;
            rh = cbt->height;
            have_layout = 1;
        } else {
            nvn_window_layout(w, &layout, &rw, &rh);
            have_layout = layout.color_count > 0 && rw && rh;
        }
        int window_target = !cbt || cbt->window_texture;
        int window_pass = !have_layout ||
                          (window_target && nvn_layout_matches_window(w, &layout, rw, rh));
        GfxOffscreenTarget *rt = NULL;
        if (have_layout && !window_pass) {
            rt = nvn_host_rt_get(rw, rh, &layout);
            if (!rt) {
                nvn_fail("nvnQueueSubmitCommands", "cannot create host render-target layout");
                continue;
            }
        }

        GfxVertexLayout input;
        uint64_t vertex_sig = 0;
        nvn_build_vertex_layout(cb, &input, &vertex_sig);
        GfxReflectedPipeline *reflected = has_program
            ? (GfxReflectedPipeline *)p->gfx_reflected_pipeline : NULL;
        if (!reflected && has_program) {
            char variant[192];
            nvn_variant_suffix(p, cb, variant, sizeof variant);
            reflected = nvn_program_cached_pipeline(p, nvn_window_format(w),
                                                    have_layout ? &layout : NULL,
                                                    window_pass,
                                                    input.nattributes ? &input : NULL,
                                                    vertex_sig, variant);
        }

        int drew = 0;
        if (reflected) {
            uint32_t cap = cb->ncmds ? cb->ncmds : 1;
            GfxCmd *translated = calloc(cap, sizeof *translated);
            if (!translated) {
                nvn_fail("nvnQueueSubmitCommands", "out of memory for translated commands");
                continue;
            }
            float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
            int has_clear = 0;
            uint32_t count = nvn_translate_commands(cb, translated, cap,
                                                    window_pass ? NULL : rt,
                                                    clear, &has_clear);
            nvn_apply_bind_state(p, reflected, cb);
            GfxVertexStream streams[16];
            uint32_t nstreams = nvn_build_vertex_streams(cb, streams, 16);
            if (window_pass)
                drew = runtime_native_window_execute_reflected_ex(w->native, reflected,
                                                                  translated, count,
                                                                  has_clear ? clear : NULL,
                                                                  streams, nstreams);
            else
                drew = gfx_backend_reflected_execute_ex(reflected, rt, translated, count,
                                                        has_clear ? clear : NULL,
                                                        streams, nstreams);
            free(translated);
            if (drew && !window_pass) nvn_window_mirror_target(w, rt);
        } else if (has_program && p->textured && p->gfx_textured_pipeline && p->gfx_texture) {
            drew = runtime_native_window_draw_textured_triangle(w->native, p->gfx_textured_pipeline,
                                                                p->gfx_texture);
        } else if (has_program && p->gfx_pipeline) {
            drew = runtime_native_window_draw_triangle_pipeline(w->native, p->gfx_pipeline);
        } else {
            /* Clear-only command buffer, or a program whose shaders are not in
             * the cache: honour the recorded clear and present a real frame
             * instead of inventing a shader. */
            float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
            int has_clear = 0;
            GfxCmd ignored[1];
            (void)nvn_translate_commands(cb, ignored, 1, NULL, clear, &has_clear);
            if (!window_pass && rt)
                drew = gfx_backend_offscreen_clear(rt, clear[0], clear[1], clear[2], clear[3]);
            else
                drew = runtime_native_window_clear_rgba(w->native, clear);
            if (drew && !window_pass) nvn_window_mirror_target(w, rt);
            static unsigned fallback_logged;
            if (fallback_logged++ < 8)
                fprintf(stderr, "[totk-nvn] no cached pipeline for program=%p: clear-only frame\n",
                        (void *)p);
        }
        if (!drew) {
            nvn_fail("nvnQueueSubmitCommands", "Vulkan draw failed");
            continue;
        }
        static int l2_frame_dumped;
        const char *l2_frame = getenv("TOTK_ANDROID_L2_FRAME");
        if (l2_frame && *l2_frame && !l2_frame_dumped) {
            l2_frame_dumped = 1;
            int shot = runtime_native_window_screenshot(w->native, l2_frame);
            fprintf(stderr, "[totk-nvn] first reflected frame -> %s (%s)\n",
                    l2_frame, shot ? "ok" : "failed");
        }
        unsigned total = __atomic_add_fetch(&nvn_nvn_draws, 1, __ATOMIC_RELAXED);
        if (total <= 32 || nvn_trace())
            fprintf(stderr, "[totk-nvn] NVN draw #%u program=%p window=%p vertices=%d%s%s\n",
                    total, (void *)p, (void *)w, cb->draw_count,
                    reflected ? " reflected" : " fallback",
                    window_pass ? "" : " mrt");
    }
}

int runtime_nvn_draw_selftest(const char *screenshot, int frames,
                              const char *vs_path, const char *ps_path, int expect_visible) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0x88);
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
    qb.dev = &d;
    qb.flags = 1;
    qb.cmd_sz = 0x10000;
    qb.ctl_sz = 0x1000;
    qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) return 0;
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    if (vs_path && *vs_path && ps_path && *ps_path) {
        if (!runtime_nvn_program_load_spirv(&p, vs_path, ps_path)) return 0;
    } else {
        p.gfx_pipeline = gfx_backend_builtin_triangle_pipeline();
    }
    if (!p.gfx_pipeline) return 0;
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d;
    wb.native = nw;
    wb.ntex = 1;
    wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
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
    int ok = expect_visible ? (center[0] > 200 && center[1] > 60 && center[1] < 100 &&
                               center[2] < 40 && corner[0] < 40) : 1;
    if (!expect_visible)
        fprintf(stderr, "[totk-nvn] translated SPIR-V draw executed; pixel visibility not asserted\n");
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] NVN draw selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x\n",
                center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
    return ok;
}

/* Regression check for the NVN command-buffer submission registry.  The guest
 * keeps many command buffers alive at once and recycles their storage after
 * nvnCommandBufferFinalize, so a submitted handle must never resolve to a
 * finalized or recycled object. */
int runtime_nvn_cb_lifetime_selftest(void) {
    enum { LIVE = 96 };
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xC8);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) { runtime_native_window_destroy(nw); return 0; }
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) { runtime_native_window_destroy(nw); return 0; }
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) { runtime_native_window_destroy(nw); return 0; }
    p.gfx_pipeline = gfx_backend_builtin_triangle_pipeline();
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) { runtime_native_window_destroy(nw); return 0; }
    cb_t *cbs = calloc(LIVE, sizeof *cbs);
    if (!cbs) { runtime_native_window_destroy(nw); return 0; }

    /* More live command buffers than the old fixed table held (64). */
    unsigned before = nvn_nvn_draws;
    for (int i = 0; i < LIVE; i++) {
        if (!nvnCommandBufferInitialize(&cbs[i], &d)) continue;
        cbs[i].prog = &p;
        cbs[i].win = &w;
        nvnCommandBufferBeginRecording(&cbs[i]);
        nvnCommandBufferDrawArrays(&cbs[i], NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cbs[i]);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    unsigned live_draws = nvn_nvn_draws - before;
    int ok = live_draws == LIVE;

    /* A finalized handle is dead: resubmitting it must not touch the object. */
    uint64_t stale = cbs[0].handle;
    nvnCommandBufferFinalize(&cbs[0]);
    before = nvn_nvn_draws;
    nvnQueueSubmitCommands(&q, 1, &stale);
    unsigned stale_draws = nvn_nvn_draws - before;
    ok &= stale_draws == 0;

    /* The same storage is reusable and must draw again. */
    if (!nvnCommandBufferInitialize(&cbs[0], &d)) ok = 0;
    cbs[0].prog = &p;
    cbs[0].win = &w;
    nvnCommandBufferBeginRecording(&cbs[0]);
    nvnCommandBufferDrawArrays(&cbs[0], NVN_DRAW_TRIANGLES, 0, 3);
    uint64_t again = nvnCommandBufferEndRecording(&cbs[0]);
    nvnQueueSubmitCommands(&q, 1, &again);
    int reused = nvn_nvn_draws == before + 1;
    ok &= reused;

    for (int i = 0; i < LIVE; i++) nvnCommandBufferFinalize(&cbs[i]);
    fprintf(stderr, "[totk-nvn] cb lifetime selftest live=%u/%u stale_draws=%u reuse=%d ok=%d\n",
            live_draws, (unsigned)LIVE, stale_draws, reused, ok);
    free(cbs);
    nvnWindowFinalize(&w);
    nvnProgramFinalize(&p);
    runtime_native_window_destroy(nw);
    return ok;
}

int runtime_nvn_bind_selftest(void) {    tex_t tex;
    samp_t samp;
    tex_pool_t tp;
    samp_pool_t sp;
    cb_t cb;
    memset(&tex, 0, sizeof tex);
    memset(&samp, 0, sizeof samp);
    memset(&tp, 0, sizeof tp);
    memset(&sp, 0, sizeof sp);
    memset(&cb, 0, sizeof cb);
    tex.magic = NVN_MAGIC('T', 'E', 'X', 0);
    samp.magic = NVN_MAGIC('S', 'A', 'M', 'P');
    tp.magic = NVN_MAGIC('T', 'P', 'O', 'L'); tp.n = 4;
    sp.magic = NVN_MAGIC('S', 'P', 'O', 'L'); sp.n = 4;
    cb.magic = NVN_MAGIC('C', 'M', 'D', 'B');
    nvn_tex_pool_add(&tp, 3, &tex);
    nvn_samp_pool_add(&sp, 2, &samp);
    nvnCommandBufferSetTexturePool(&cb, &tp);
    nvnCommandBufferSetSamplerPool(&cb, &sp);

    uint64_t h = nvnDeviceGetTextureHandle(NULL, 3, 2);
    int ok = (h & 0xfffffu) == 3 && ((h >> 20) & 0xfffu) == 2;
    ok &= nvn_tex_pool_lookup(&tp, 3) == &tex;
    ok &= nvn_samp_pool_lookup(&sp, 2) == &samp;
    nvnCommandBufferBindTexture(&cb, 5, 0, h);
    nvnCommandBufferBindTexture(&cb, 5, 1, nvnDeviceGetSeparateTextureHandle(NULL, 3));
    nvnCommandBufferBindImage(&cb, 5, 0, nvnDeviceGetImageHandle(NULL, 3));
    nvnCommandBufferBindUniformBuffer(&cb, 0, 4, 0x1000, 0x100);
    nvnCommandBufferBindStorageBuffer(&cb, 5, 0, 0x2000, 0x100);
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] bind selftest handle=%#llx ok=%d\n",
                (unsigned long long)h, ok);
    nvn_tex_pool_forget(&tp);
    nvn_samp_pool_forget(&sp);
    return ok;
}

int runtime_nvn_draw_reflect_selftest(const char *screenshot, int frames,
                                      const char *vs_path, const char *ps_path,
                                      const char *vs_res, const char *ps_res) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xA1);
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
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    if (!runtime_nvn_program_load_spirv_reflect(&p, vs_path, ps_path, vs_res, ps_res)) return 0;
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
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
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] reflected draw selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x\n",
                center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3]);
    nvnProgramFinalize(&p);
    return 1;
}

/* --- Resource/vertex probe helpers --------------------------------------- */

typedef struct {
    uint8_t entries[2][16];
    void *controls[2];
} nvn_probe_pair_t;

static void nvn_probe_pair_free(nvn_probe_pair_t *pair) {
    if (!pair) return;
    for (unsigned i = 0; i < 2; i++) {
        free(pair->controls[i]);
        pair->controls[i] = NULL;
    }
}

/* Build a synthetic NVN control/code pair, store the translated modules under
 * the resulting keys, and bind the program.  The key derivation is the real
 * runtime path; only the control/code bytes are synthetic. */
static int nvn_probe_pair_load(prog_t *p, const uint8_t *vs, size_t vs_size,
                               const uint8_t *ps, size_t ps_size,
                               const char *vs_res, const char *ps_res,
                               uint8_t tag, nvn_probe_pair_t *pair) {
    memset(pair, 0, sizeof *pair);
    for (unsigned stage = 0; stage < 2; stage++) {
        uint8_t *control = calloc(1, 0x880);
        uint8_t *code = calloc(1, 0x100);
        if (!control || !code) { free(control); free(code); goto fail; }
        uint32_t declared = 0x880, c1 = 0, length = 0x100, st = stage;
        memcpy(control + 0x20, &declared, sizeof declared);
        memcpy(control + 0x700, &c1, sizeof c1);
        memcpy(control + 0x704, &length, sizeof length);
        memcpy(control + 0x714, &st, sizeof st);
        memset(code, (uint8_t)(tag + stage * 0x40), length);
        RuntimeShaderIdentity id;
        if (!runtime_shader_identity(control, code, &id)) {
            free(control); free(code); goto fail;
        }
        if (!runtime_shader_cache_store(runtime_shader_cache_root(), &id,
                                        stage ? ps : vs, stage ? ps_size : vs_size,
                                        stage ? ps_res : vs_res)) {
            fprintf(stderr, "[totk-nvn] ERROR store probe shader %s\n",
                    runtime_shader_stage_name(stage));
            free(control); free(code); goto fail;
        }
        uint64_t address = (uint64_t)(uintptr_t)code;
        memcpy(pair->entries[stage], &address, 8);
        memcpy(pair->entries[stage] + 8, &control, 8);
        pair->controls[stage] = control;
    }
    if (!nvnProgramSetShaders(p, 2, pair->entries)) goto fail;
    return 1;
fail:
    nvn_probe_pair_free(pair);
    return 0;
}

/* Reset descriptor state between probe sub-tests while keeping the NVN render
 * state defaults (a raw memset would disable the colour write mask). */
static void nvn_probe_cb_reset(cb_t *cb) {
    if (!cb || !cb->bind_state) return;
    nvn_bind_state_t *b = cb->bind_state;
    memset(b, 0, sizeof *b);
    nvn_render_state_defaults(&b->rs);
}

static int nvn_probe_check(const char *what, const uint8_t px[4],
                           int r_lo, int r_hi, int g_lo, int g_hi, int b_lo, int b_hi) {
    int ok = px[0] >= r_lo && px[0] <= r_hi &&
             px[1] >= g_lo && px[1] <= g_hi &&
             px[2] >= b_lo && px[2] <= b_hi;
    fprintf(stderr, "[totk-nvn] state probe %-7s center=%02x%02x%02x%02x ok=%d\n",
            what, px[0], px[1], px[2], px[3], ok);
    return ok;
}

int runtime_nvn_state_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xB1);
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
    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;

    int ok = 1;
    uint8_t center[4] = {0};

    /* 1. Uniform buffer bound through the command buffer. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x60, &pair)) {
            ok = 0;
        } else {
            float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0,
                                                  (uint64_t)(uintptr_t)color, sizeof color);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("ubo", center, 0x3c, 0x44, 0x7c, 0x84, 0xbb, 0xc3);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 1b. Two UBO epochs in one command buffer: the first draw must keep the
     * red constants even after the second draw binds blue. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d)
            || !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                    "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                    "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                    0x70, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)blue, sizeof blue);
                nvnCommandBufferSetViewport(&cb, 0, 0, 640, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 640, 720);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            uint8_t left[4] = { 0 };
            runtime_native_window_read_pixel(nw, 320, 360, left);
            runtime_native_window_read_pixel(nw, 760, 360, center);
            ok &= nvn_probe_check("orderL", left, 0x00, 0x0f, 0x00, 0x0f, 0xf0, 0xff);
            ok &= nvn_probe_check("orderR", center, 0xf0, 0xff, 0x00, 0x0f, 0x00, 0x0f);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2. Guest texture bound through the command buffer's texture pool. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_texture_modules(&vs, &vs_size, &ps, &ps_size);
        uint8_t *pool_mem = calloc(1, 0x10000);
        pool_t pool;
        pool_builder_t pb;
        tex_builder_t tb;
        tex_t tex;
        tex_pool_t tp;
        memset(&pool, 0, sizeof pool);
        memset(&pb, 0, sizeof pb);
        memset(&tex, 0, sizeof tex);
        memset(&tp, 0, sizeof tp);
        int have_tex = 0;
        if (pool_mem) {
            pb.dev = &d;
            pb.mem = pool_mem;
            pb.size = 0x10000;
            pb.flags = 1;
            have_tex = nvnMemoryPoolInitialize(&pool, &pb) != 0;
        }
        if (have_tex) {
            nvnTextureBuilderSetDefaults(&tb);
            tb.dev = &d;
            nvnTextureBuilderSetSize2D(&tb, 2, 2);
            nvnTextureBuilderSetFormat(&tb, 0x25);
            nvnTextureBuilderSetLevels(&tb, 1);
            nvnTextureBuilderSetStorage(&tb, &pool, 0);
            have_tex = nvnTextureInitialize(&tex, &tb) != 0;
        }
        static const uint8_t checker[16] = {
            255, 0, 255, 255,   255, 0, 255, 255,
            255, 0, 255, 255,   255, 0, 255, 255,
        };
        if (have_tex) {
            int sw = runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                                   tex.d.flags, tex.d.samples, tex.d.width,
                                                   tex.d.height, tex.d.depth, tex.d.levels,
                                                   tex.d.stride, tex.d.pkg_block_h);
            int up = sw == 0 ? runtime_nvn_texture_upload_guest(&tex) : 0;
            have_tex = sw == 0 && up && nvn_texture_gfx(&tex) != NULL;
        }
        if (have_tex)
            have_tex = nvnTexturePoolInitialize(&tp, &pool, 0, 4) != 0;
        if (have_tex) nvnTexturePoolRegisterTexture(&tp, 1, &tex, NULL);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!have_tex || !nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[],\"sbufs\":[],"
                                 "\"textures\":[{\"binding\":0,\"slot\":0,\"type\":3,"
                                 "\"format\":\"R8G8B8A8Unorm\",\"array\":1,"
                                 "\"separate\":false,\"flags\":0}],\"images\":[]}",
                                 0x80, &pair)) {
            ok = 0;
        } else {
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                nvnCommandBufferSetTexturePool(&cb, &tp);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindTexture(&cb, 1, 0, 1);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("texture", center, 0xf0, 0xff, 0x00, 0x0f, 0xf0, 0xff);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
        if (have_tex) {
            nvnTexturePoolFinalize(&tp);
            nvnTextureFinalize(&tex);
        }
        free(pool_mem);
    }

    /* 2b. Combined texture handle selects the sampler-pool entry: the default
     * texture sampler is linear, so nearest must produce a corner texel. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_texture_modules(&vs, &vs_size, &ps, &ps_size);
        uint8_t *pool_mem = calloc(1, 0x10000);
        pool_t pool;
        pool_builder_t pb;
        tex_builder_t tb;
        tex_t tex;
        tex_pool_t tp;
        samp_builder_t sb;
        samp_t samp;
        samp_pool_t sp;
        memset(&pool, 0, sizeof pool);
        memset(&pb, 0, sizeof pb);
        memset(&tex, 0, sizeof tex);
        memset(&tp, 0, sizeof tp);
        memset(&samp, 0, sizeof samp);
        memset(&sp, 0, sizeof sp);
        int have_tex = 0, have_samp = 0;
        if (pool_mem) {
            pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
            have_tex = nvnMemoryPoolInitialize(&pool, &pb) != 0;
        }
        if (have_tex) {
            nvnTextureBuilderSetDefaults(&tb);
            tb.dev = &d;
            nvnTextureBuilderSetSize2D(&tb, 2, 2);
            nvnTextureBuilderSetFormat(&tb, 0x25);
            nvnTextureBuilderSetLevels(&tb, 1);
            nvnTextureBuilderSetStorage(&tb, &pool, 0);
            have_tex = nvnTextureInitialize(&tex, &tb) != 0;
        }
        static const uint8_t checker[16] = {
            255, 0, 0, 255,   0, 255, 0, 255,
            0, 0, 255, 255,   255, 255, 255, 255,
        };
        if (have_tex) {
            int sw = runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                                   tex.d.flags, tex.d.samples, tex.d.width,
                                                   tex.d.height, tex.d.depth, tex.d.levels,
                                                   tex.d.stride, tex.d.pkg_block_h);
            have_tex = sw == 0 && runtime_nvn_texture_upload_guest(&tex) && nvn_texture_gfx(&tex);
        }
        if (have_tex) have_tex = nvnTexturePoolInitialize(&tp, &pool, 0, 4) != 0;
        if (have_tex) nvnTexturePoolRegisterTexture(&tp, 1, &tex, NULL);
        if (have_tex) {
            nvnSamplerBuilderSetDefaults(&sb);
            sb.dev = &d;
            nvnSamplerBuilderSetMinMagFilter(&sb, 0, 0);   /* NEAREST / NEAREST */
            nvnSamplerBuilderSetWrapMode(&sb, 7, 7, 7);    /* CLAMP_TO_EDGE */
            have_samp = nvnSamplerInitialize(&samp, &sb) != 0;
        }
        if (have_samp)
            have_samp = nvnSamplerPoolInitialize(&sp, &pool, 0x1000, 4) != 0;
        if (have_samp) nvnSamplerPoolRegisterSampler(&sp, 1, &samp);
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!have_tex || !have_samp || !nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[],\"sbufs\":[],"
                                 "\"textures\":[{\"binding\":0,\"slot\":0,\"type\":3,"
                                 "\"format\":\"R8G8B8A8Unorm\",\"array\":1,"
                                 "\"separate\":false,\"flags\":0}],\"images\":[]}",
                                 0x90, &pair)) {
            ok = 0;
        } else {
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                nvnCommandBufferSetTexturePool(&cb, &tp);
                nvnCommandBufferSetSamplerPool(&cb, &sp);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindTexture(&cb, 1, 0, (1u << 20) | 1u);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            int nearest = (center[0] == 0 || center[0] == 255) &&
                          (center[1] == 0 || center[1] == 255) &&
                          (center[2] == 0 || center[2] == 255) &&
                          (center[0] + center[1] + center[2] >= 255);
            fprintf(stderr, "[totk-nvn] state probe sampler center=%02x%02x%02x%02x ok=%d\n",
                    center[0], center[1], center[2], center[3], nearest);
            ok &= nearest;
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
        if (have_samp) nvnSamplerPoolFinalize(&sp);
        if (have_tex) {
            nvnTexturePoolFinalize(&tp);
            nvnTextureFinalize(&tex);
        }
        if (have_samp) nvnSamplerFinalize(&samp);
        free(pool_mem);
    }

    /* 2c. Depth test: a farther triangle must not overwrite a nearer one. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_depth_modules(&vs, &vs_size, &ps, &ps_size);
        struct ZVert { float x, y, z; };
        static const struct ZVert verts[9] = {
            { -0.65f, -0.65f, 0.50f }, { 0.65f, -0.65f, 0.50f }, { 0.0f, 0.65f, 0.50f },
            { -0.65f, -0.65f, 0.75f }, { 0.65f, -0.65f, 0.75f }, { 0.0f, 0.65f, 0.75f },
            { -0.65f, -0.65f, 0.25f }, { 0.65f, -0.65f, 0.25f }, { 0.0f, 0.65f, 0.25f },
        };
        uint32_t va[2] = {
            (uint32_t)0x16 | (0u << 8) | (0u << 27),
            (uint32_t)0x0A | (8u << 8) | (0u << 27),
        };
        uint32_t vstream[2] = { (uint32_t)sizeof(struct ZVert), 0 };
        nvn_ds_st ds;
        nvnDepthStencilStateSetDefaults(&ds);
        nvnDepthStencilStateSetDepthTestEnable(&ds, 1);
        nvnDepthStencilStateSetDepthWriteEnable(&ds, 1);
        nvnDepthStencilStateSetDepthFunc(&ds, 2);   /* LESS */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x30, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
            float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            for (int frame = 0; frame < 2; frame++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferClearDepthStencil(&cb, 1.0f, 1, 0, 0);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindVertexBuffer(&cb, 0, (uint64_t)(uintptr_t)verts, sizeof verts);
                nvnCommandBufferBindVertexAttribState(&cb, 2, va);
                nvnCommandBufferBindVertexStreamState(&cb, 1, vstream);
                nvnCommandBufferBindDepthStencilState(&cb, &ds);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)
                                                  (frame ? blue : green),
                                                  sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, frame ? 6 : 3, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
                runtime_native_window_read_pixel(nw, 640, 360, center);
                if (frame == 0)
                    ok &= nvn_probe_check("depth", center, 0xf0, 0xff, 0x00, 0x0f, 0x00, 0x0f);
                else
                    ok &= nvn_probe_check("depthN", center, 0x00, 0x0f, 0x00, 0x0f, 0xf0, 0xff);
            }
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2d. Blend applies only to the second draw (per-draw pipeline variant). */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        nvn_color_st color;
        nvn_blend_st blend;
        nvnColorStateSetDefaults(&color);
        nvnBlendStateSetDefaults(&blend);
        nvnColorStateSetBlendEnable(&color, 0, 1);
        nvnBlendStateSetBlendFunc(&blend, 5, 6, 5, 6);   /* SRC_ALPHA / 1-SRC_ALPHA */
        nvnBlendStateSetBlendEquation(&blend, 1, 1);     /* ADD */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x40, &pair)) {
            ok = 0;
        } else {
            float green[4] = { 0.0f, 1.0f, 0.0f, 0.5f };
            float red[4] = { 1.0f, 0.0f, 0.0f, 0.5f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)green, sizeof green);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                nvnCommandBufferBindColorState(&cb, &color);
                nvnCommandBufferBindBlendState(&cb, &blend);
                nvnCommandBufferSetBlendColor(&cb, red);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("blend", center, 0x78, 0x88, 0x78, 0x88, 0x00, 0x08);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 2e. Raster cull: front-facing CCW triangle with front-face culling. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_ubo_modules(&vs, &vs_size, &ps, &ps_size);
        nvn_poly_st poly;
        nvnPolygonStateSetDefaults(&poly);
        nvnPolygonStateSetCullFace(&poly, 2);    /* BACK */
        nvnPolygonStateSetFrontFace(&poly, 1);   /* CCW */
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL,
                                 "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
                                 "\"sbufs\":[],\"textures\":[],\"images\":[]}",
                                 0x50, &pair)) {
            ok = 0;
        } else {
            float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindPolygonState(&cb, &poly);
                nvnCommandBufferBindUniformBuffer(&cb, 1, 0, (uint64_t)(uintptr_t)red, sizeof red);
                nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("cull", center, 0x0d, 0x14, 0x14, 0x1f, 0x1f, 0x24);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    /* 3. Guest vertex stream + attributes + indexed draw. */
    {
        const uint8_t *vs = NULL, *ps = NULL;
        size_t vs_size = 0, ps_size = 0;
        gfx_backend_builtin_attr_modules(&vs, &vs_size, &ps, &ps_size);
        struct Vertex { float x, y, u, v; };
        static const struct Vertex verts[3] = {
            { -0.65f, -0.65f, 0.0f, 0.0f },
            {  0.65f, -0.65f, 1.0f, 0.0f },
            {  0.0f,   0.65f, 0.5f, 1.0f },
        };
        static const uint16_t indices[3] = { 0, 1, 2 };
        uint32_t va[2] = {
            (uint32_t)0x16 | (0u << 8) | (0u << 27),
            (uint32_t)0x16 | (8u << 8) | (0u << 27),
        };
        uint32_t vstream[2] = { (uint32_t)sizeof(struct Vertex), 0 };
        prog_t p;
        memset(&p, 0, sizeof p);
        nvn_probe_pair_t pair;
        memset(&pair, 0, sizeof pair);
        if (!nvnProgramInitialize(&p, &d) ||
            !nvn_probe_pair_load(&p, vs, vs_size, ps, ps_size, NULL, NULL, 0xA0, &pair)) {
            ok = 0;
        } else {
            for (int i = 0; i < frames; i++) {
                nvn_probe_cb_reset(&cb);
                cb.tex_pool = NULL;
                cb.draw_count = 0;
                nvnCommandBufferBeginRecording(&cb);
                nvnCommandBufferBindProgram(&cb, &p, -1);
                nvnCommandBufferSetWindow(&cb, &w);
                const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
                nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
                nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
                nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
                nvnCommandBufferBindVertexBuffer(&cb, 0, (uint64_t)(uintptr_t)verts, sizeof verts);
                nvnCommandBufferBindVertexAttribState(&cb, 2, va);
                nvnCommandBufferBindVertexStreamState(&cb, 1, vstream);
                nvnCommandBufferDrawElements(&cb, NVN_DRAW_TRIANGLES, 1, 3,
                                             (uint64_t)(uintptr_t)indices);
                uint64_t h = nvnCommandBufferEndRecording(&cb);
                nvnQueueSubmitCommands(&q, 1, &h);
                nvnQueuePresentTexture(&q, &w, 0);
            }
            runtime_native_window_read_pixel(nw, 640, 360, center);
            ok &= nvn_probe_check("attr", center, 0x70, 0x90, 0x70, 0x90, 0x30, 0x50);
        }
        nvnProgramFinalize(&p);
        nvn_probe_pair_free(&pair);
    }

    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    fprintf(stderr, "[totk-nvn] state selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int runtime_nvn_frame_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xF1);
    if (!nw) return 0;

    /* Synthetic NVN control/code pairs give the cache real keys without game
     * data.  The translated modules are the repository's triangle pair. */
    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    if (!vs || !ps) { runtime_native_window_destroy(nw); return 0; }
    static const char res_json[] =
        "{\"cbufs\":[],\"sbufs\":[],\"textures\":[],\"images\":[]}";
    int miss = getenv("TOTK_ANDROID_SHADER_CACHE_MISS") != NULL;
    int clear_only = getenv("TOTK_ANDROID_CLEAR_ONLY") != NULL;
    RuntimeShaderIdentity ids[2];
    uint8_t *shader_entries = calloc(2, 16);
    void *shader_controls[2] = {0};
    if (!shader_entries) { runtime_native_window_destroy(nw); return 0; }
    for (unsigned stage = 0; stage < 2; stage++) {
        uint8_t *control = calloc(1, 0x880);
        uint8_t *code = calloc(1, 0x100);
        if (!control || !code) { free(control); free(code); runtime_native_window_destroy(nw); return 0; }
        uint32_t declared = 0x880, c1 = 0, length = 0x100, st = stage;
        memcpy(control + 0x20, &declared, sizeof declared);
        memcpy(control + 0x700, &c1, sizeof c1);
        memcpy(control + 0x704, &length, sizeof length);
        memcpy(control + 0x714, &st, sizeof st);
        memset(code, (uint8_t)((stage ? 0x22 : 0x11) ^ (miss ? 0x33 : 0)), length);
        if (!runtime_shader_identity(control, code, &ids[stage])) {
            fprintf(stderr, "[totk-nvn] ERROR synthetic shader identity stage=%u\n", stage);
            free(control); free(code); runtime_native_window_destroy(nw); return 0;
        }
        if (!miss && !runtime_shader_cache_store(runtime_shader_cache_root(), &ids[stage],
                                                 stage ? ps : vs, stage ? ps_size : vs_size,
                                                 res_json)) {
            fprintf(stderr, "[totk-nvn] ERROR write shader cache root=%s\n",
                    runtime_shader_cache_root());
            free(control); free(code); runtime_native_window_destroy(nw); return 0;
        }
        fprintf(stderr, "[totk-nvn] frame probe cache %s %s code=%u%s\n",
                runtime_shader_stage_name(stage), ids[stage].key, length,
                miss ? " (miss test, not stored)" : "");
        /* NVNshaderData in memory: {uint64 data, void *control}. */
        uint8_t *entry = shader_entries + stage * 16;
        uint64_t addr = (uint64_t)(uintptr_t)code;
        memcpy(entry, &addr, 8);
        memcpy(entry + 8, &control, 8);
        shader_controls[stage] = control;
    }

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

    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    if (!nvnProgramSetShaders(&p, 2, shader_entries)) return 0;

    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d;
    wb.native = nw;
    wb.ntex = 1;
    wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) return 0;

    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    for (int i = 0; i < frames; i++) {
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferSetProgram(&cb, &p);
        nvnCommandBufferSetWindow(&cb, &w);
        const float clear[4] = { 0.05f, 0.08f, 0.12f, 1.0f };
        nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
        nvnCommandBufferSetViewport(&cb, 0, 0, 1280, 720);
        nvnCommandBufferSetScissor(&cb, 0, 0, 1280, 720);
        if (!clear_only) nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &handle);
        nvnQueuePresentTexture(&q, &w, 0);
    }
    nvnCommandBufferFinalize(&cb);
    uint8_t center[4] = {0}, corner[4] = {0};
    runtime_native_window_read_pixel(nw, 640, 360, center);
    runtime_native_window_read_pixel(nw, 0, 0, corner);
    if (screenshot && *screenshot) runtime_native_window_screenshot(nw, screenshot);
    int ok = center[0] > 200 && center[1] > 60 && center[1] < 100 && center[2] < 40 &&
             corner[0] < 40;
    if (miss) {
        /* No cached pipeline: the frame is the recorded clear, not a draw. */
        ok = center[0] < 40 && center[1] < 40 && center[2] < 40 &&
             center[0] == corner[0] && center[1] == corner[1] && center[2] == corner[2];
    }
    if (clear_only) {
        ok = center[0] < 40 && center[1] < 40 && center[2] < 40 &&
             center[0] == corner[0] && center[1] == corner[1] && center[2] == corner[2];
    }
    fprintf(stderr, "[totk-nvn] frame selftest center=%02x%02x%02x%02x corner=%02x%02x%02x%02x ok=%d%s\n",
            center[0], center[1], center[2], center[3],
            corner[0], corner[1], corner[2], corner[3], ok,
            miss ? " (cache miss fallback)" : clear_only ? " (clear-only)" : "");

    nvnWindowFinalize(&w);
    nvnProgramFinalize(&p);
    free(shader_entries);
    for (unsigned stage = 0; stage < 2; stage++) free(shader_controls[stage]);
    return ok;
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
    static const uint8_t checker[16] = {
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
    p.gfx_textured_pipeline = tp;
    p.gfx_texture = tex_gfx;
    p.textured = 1;
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

int runtime_nvn_texture_bind_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 60) frames = 60;
    void *nw = runtime_native_window_create(1280, 720, 0x25, 0xA2);
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
    static const uint8_t checker[16] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 255,
    };
    if (runtime_guest_texture_swizzle(tex.mem, checker, 8, tex.d.target, tex.d.format,
                                       tex.d.flags, tex.d.samples, tex.d.width, tex.d.height,
                                       tex.d.depth, tex.d.levels, tex.d.stride,
                                       tex.d.pkg_block_h)) { free(pool_mem); return 0; }
    if (!runtime_nvn_texture_upload_guest(&tex)) { free(pool_mem); return 0; }

    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) { free(pool_mem); return 0; }
    p.gfx_reflected_pipeline = gfx_backend_reflected_builtin_texture_pipeline_create();
    if (!p.gfx_reflected_pipeline) { free(pool_mem); return 0; }
    if (!runtime_nvn_program_bind_texture(&p, 1, 0, &tex)) {
        fprintf(stderr, "[totk-nvn] ERROR bind real texture to reflected PS\n");
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }

    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (!nvnWindowInitialize(&w, &wb)) {
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) {
        nvnProgramFinalize(&p); free(pool_mem); return 0;
    }
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
    int visible = center[0] != corner[0] || center[1] != corner[1] ||
                  center[2] != corner[2] || center[3] != corner[3];
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] textured reflected selftest center=%02x%02x%02x%02x"
                        " corner=%02x%02x%02x%02x visible=%d\n",
                center[0], center[1], center[2], center[3],
                corner[0], corner[1], corner[2], corner[3], visible);
    nvnProgramFinalize(&p);
    free(pool_mem);
    return visible;
}

static int nvn_probe_px_near(const uint8_t a[4], const float c[4], int tol) {
    for (int i = 0; i < 4; i++) {
        int want = (int)(c[i] * 255.0f + 0.5f);
        if (a[i] < want - tol || a[i] > want + tol) return 0;
    }
    return 1;
}

/* probe-nvn-mrt: two color attachments plus depth.  Clear, draw, then read
 * both attachments back through the layout-aware readback API.  Also checks
 * that the same program produces distinct pipelines for a 1-target and a
 * 2-target layout (the MRT layout is part of the pipeline key). */
int runtime_nvn_mrt_selftest(const char *screenshot, int frames) {
    if (frames < 1) frames = 1;
    if (frames > 16) frames = 16;
    int color_fmt = gfx_backend_vk_format_for_nvn(0x25);   /* RGBA8 */
    int depth_fmt = gfx_backend_vk_format_for_nvn(0x35);   /* D24S8 */
    if (!color_fmt || !depth_fmt) {
        fprintf(stderr, "[totk-nvn] mrt probe: format mapping unavailable\n");
        return 0;
    }
    GfxRenderTargetLayout mrt;
    memset(&mrt, 0, sizeof mrt);
    mrt.color_count = 2;
    mrt.color_formats[0] = color_fmt;
    mrt.color_formats[1] = color_fmt;
    mrt.depth_format = depth_fmt;
    GfxRenderTargetLayout single;
    memset(&single, 0, sizeof single);
    single.color_count = 1;
    single.color_formats[0] = color_fmt;

    GfxOffscreenTarget *target = gfx_backend_offscreen_create_layout(320, 240, &mrt);
    if (!target) {
        fprintf(stderr, "[totk-nvn] mrt probe: offscreen layout creation failed\n");
        return 0;
    }
    int ok = gfx_backend_offscreen_color_count(target) == 2;
    const GfxRenderTargetLayout *got = gfx_backend_offscreen_layout(target);
    ok &= got && got->color_count == 2 && got->depth_format == depth_fmt &&
          got->color_formats[1] == color_fmt;

    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    if (!vs || !ps) { gfx_backend_offscreen_destroy(target); return 0; }
    GfxReflectedPipeline *mrt_pipe =
        gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size, &mrt,
                                                     NULL, 0, NULL, 0, NULL);
    GfxReflectedPipeline *single_pipe =
        gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size, &single,
                                                     NULL, 0, NULL, 0, NULL);
    if (!mrt_pipe || !single_pipe) {
        fprintf(stderr, "[totk-nvn] mrt probe: pipeline creation failed (mrt=%p single=%p)\n",
                (void *)mrt_pipe, (void *)single_pipe);
        ok = 0;
    } else {
        int distinct = mrt_pipe != single_pipe;
        fprintf(stderr, "[totk-nvn] mrt probe pipelines mrt=%p single=%p distinct=%d\n",
                (void *)mrt_pipe, (void *)single_pipe, distinct);
        ok &= distinct;
    }
    if (ok && mrt_pipe) {
        const float clear[4] = { 0.10f, 0.20f, 0.40f, 1.0f };
        for (int f = 0; f < frames; f++) {
            GfxCmd cmds[8];
            memset(cmds, 0, sizeof cmds);
            uint32_t nc = 0;
            cmds[nc].kind = GFX_CMD_CLEAR_COLOR;
            memcpy(cmds[nc].u.clear.rgba, clear, sizeof clear);
            cmds[nc].u.clear.mask = 0xf;
            nc++;
            cmds[nc].kind = GFX_CMD_CLEAR_DEPTH_STENCIL;
            cmds[nc].u.clear_depth.depth = 1.0f;
            cmds[nc].u.clear_depth.dmask = 1;
            cmds[nc].u.clear_depth.stencil = 0;
            cmds[nc].u.clear_depth.smask = 0xff;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_RENDER_TARGETS;
            cmds[nc].u.set_targets.target = target;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_VIEWPORT;
            cmds[nc].u.rect.x = 0; cmds[nc].u.rect.y = 0;
            cmds[nc].u.rect.width = 320; cmds[nc].u.rect.height = 240;
            nc++;
            cmds[nc].kind = GFX_CMD_SET_SCISSOR;
            cmds[nc].u.rect.x = 0; cmds[nc].u.rect.y = 0;
            cmds[nc].u.rect.width = 320; cmds[nc].u.rect.height = 240;
            nc++;
            cmds[nc].kind = GFX_CMD_DRAW;
            cmds[nc].u.draw.primitive = NVN_DRAW_TRIANGLES;
            cmds[nc].u.draw.first = 0;
            cmds[nc].u.draw.count = 3;
            cmds[nc].u.draw.has_state = 0;
            nc++;
            if (!gfx_backend_reflected_execute_ex(mrt_pipe, target, cmds, nc, clear, NULL, 0)) {
                fprintf(stderr, "[totk-nvn] mrt probe: execute failed\n");
                ok = 0;
                break;
            }
        }
    }

    uint8_t c0[4] = {0}, c1[4] = {0}, k0[4] = {0}, k1[4] = {0};
    if (ok) {
        ok &= gfx_backend_offscreen_read_pixel_index(target, 0, 160, 120, c0) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 1, 160, 120, c1) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 0, 4, 4, k0) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(target, 1, 4, 4, k1) != 0;
    }
    /* Attachment 0 carries the triangle (bright red-ish); attachment 1 was only
     * cleared, so it must hold the pass clear colour. */
    int drawn = c0[0] > 180 && c0[1] < 120 && c0[2] < 120;
    int cleared1 = nvn_probe_px_near(c1, (const float[4]){ 0.10f, 0.20f, 0.40f, 1.0f }, 6);
    int cleared1_corner = nvn_probe_px_near(k1, (const float[4]){ 0.10f, 0.20f, 0.40f, 1.0f }, 6);
    ok &= drawn && cleared1 && cleared1_corner;
    fprintf(stderr, "[totk-nvn] mrt probe color0=%02x%02x%02x%02x color1=%02x%02x%02x%02x"
                    " corner1=%02x%02x%02x%02x drawn=%d cleared1=%d\n",
            c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3],
            k1[0], k1[1], k1[2], k1[3], drawn, cleared1);

    if (screenshot && *screenshot) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s.c0.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(target, 0, path)) ok = 0;
        snprintf(path, sizeof path, "%s.c1.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(target, 1, path)) ok = 0;
    }
    if (mrt_pipe) gfx_backend_reflected_pipeline_destroy(mrt_pipe);
    if (single_pipe) gfx_backend_reflected_pipeline_destroy(single_pipe);
    gfx_backend_offscreen_destroy(target);
    fprintf(stderr, "[totk-nvn] mrt selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* probe-nvn-fence: submit -> fence -> map/read ordering.  A fence may only be
 * signaled after the queue reports completion, and a mapped buffer must see
 * the storage only after that completion. */
int runtime_nvn_fence_selftest(void) {
    void *nw = runtime_native_window_create(320, 240, 0x25, 0xE1);
    if (!nw) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    queue_t q;
    queue_builder_t qb;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    int ok = nvnDeviceInitialize(&d, &db) != 0;
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    ok &= nvnQueueInitialize(&q, &qb) != 0;

    uint8_t *pool_mem = calloc(1, 0x10000);
    pool_t pool;
    pool_builder_t pb;
    buf_t buf;
    buf_builder_t bb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    memset(&buf, 0, sizeof buf);
    memset(&bb, 0, sizeof bb);
    if (pool_mem) {
        pb.dev = &d; pb.mem = pool_mem; pb.size = 0x10000; pb.flags = 1;
        ok &= nvnMemoryPoolInitialize(&pool, &pb) != 0;
        bb.dev = &d; bb.pool = &pool; bb.off = 0; bb.size = 0x100;
        ok &= nvnBufferInitialize(&buf, &bb) != 0;
    } else {
        ok = 0;
    }
    /* CPU writes before the submit are the data the post-map reader checks. */
    static const uint8_t pattern[8] = { 0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67 };
    if (ok) {
        uint8_t *m = nvnBufferMap(&buf);
        if (!m) ok = 0; else memcpy(m, pattern, sizeof pattern);
    }

    win_t w;
    win_builder_t wb;
    memset(&w, 0, sizeof w);
    memset(&wb, 0, sizeof wb);
    wb.dev = &d; wb.native = nw; wb.ntex = 1; wb.tex[0] = (void *)(uintptr_t)1;
    if (ok) ok = nvnWindowInitialize(&w, &wb) != 0;
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (ok) ok = nvnCommandBufferInitialize(&cb, &d) != 0;

    sync_t fence_sync;
    memset(&fence_sync, 0, sizeof fence_sync);
    if (ok) ok = nvnSyncInitialize(&fence_sync, &d) != 0;

    /* Submit a real frame (clear through the host target). */
    if (ok) {
        cb.win = &w;
        nvnCommandBufferBeginRecording(&cb);
        const float clear[4] = { 0.15f, 0.35f, 0.55f, 1.0f };
        nvnCommandBufferClearColor(&cb, 0, clear, 0xf);
        uint64_t h = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &h);
        nvnQueuePresentTexture(&q, &w, 0);
    }

    int fence_ok = 0, wait_ok = 0, queue_wait_ok = 0;
    if (ok) {
        fence_ok = nvnQueueFenceSync(&q, &fence_sync, 0 /* NVN_SYNC_CONDITION_ALL_GPU_WORK */, 0) != 0;
        int r = nvnSyncWait(&fence_sync, 1000000000ull);
        wait_ok = r == NVN_SYNC_WAIT_CONDITION_SATISFIED || r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
        if (r == NVN_SYNC_WAIT_TIMEOUT_EXPIRED)
            fprintf(stderr, "[totk-nvn] fence probe: SyncWait timed out\n");
        queue_wait_ok = nvnQueueWaitSync(&q, &fence_sync) != 0;
    }

    int map_ok = 0, pattern_ok = 0;
    if (ok) {
        uint8_t *m = nvnBufferMap(&buf);
        map_ok = m != NULL;
        if (m) pattern_ok = memcmp(m, pattern, sizeof pattern) == 0;
        uint8_t *pm = nvnMemoryPoolMap(&pool);
        map_ok = map_ok && pm != NULL && pm == (uint8_t *)m;
    }

    int acquire_ok = 0, acquire_sync_ok = 0;
    int index = -1;
    if (ok) {
        acquire_ok = nvnQueueAcquireTexture(&q, &w, &index) == 0 && index >= 0 && index < w.ntex;
        sync_t acq;
        memset(&acq, 0, sizeof acq);
        if (nvnSyncInitialize(&acq, &d)) {
            if (nvnWindowAcquireTexture(&w, &acq, &index) == 0) {
                int r = nvnSyncWait(&acq, 1000000000ull);
                acquire_sync_ok = r == NVN_SYNC_WAIT_CONDITION_SATISFIED ||
                                  r == NVN_SYNC_WAIT_ALREADY_SIGNALED;
            }
            nvnSyncFinalize(&acq);
        }
    }
    uint8_t px[4] = {0};
    if (ok) runtime_native_window_read_pixel(nw, 160, 120, px);

    /* A guest window texture (nvnWindowBuilderSetTextures) must drive the host
     * target size/format instead of the 1920x1080 RGBA8 fallback. */
    int window_layout_ok = 0;
    if (ok) {
        tex_builder_t tb;
        tex_t wtex;
        memset(&wtex, 0, sizeof wtex);
        nvnTextureBuilderSetDefaults(&tb);
        tb.dev = &d;
        nvnTextureBuilderSetSize2D(&tb, 64, 64);
        nvnTextureBuilderSetFormat(&tb, 0x25);
        nvnTextureBuilderSetLevels(&tb, 1);
        nvnTextureBuilderSetStorage(&tb, &pool, 0x100);
        win_t gw;
        win_builder_t wb2;
        memset(&gw, 0, sizeof gw);
        memset(&wb2, 0, sizeof wb2);
        wb2.dev = &d;
        wb2.native = NULL;              /* force the host to create the target */
        wb2.ntex = 1;
        wb2.tex[0] = &wtex;
        if (nvnTextureInitialize(&wtex, &tb) && nvnWindowInitialize(&gw, &wb2)) {
            uint32_t gwid = 0, ghei = 0, gfmt = 0;
            if (runtime_native_window_info(nvnWindowGetNativeWindow(&gw),
                                           &gwid, &ghei, &gfmt, NULL, NULL))
                window_layout_ok = gwid == 64 && ghei == 64 && gfmt == 0x25;
            nvnWindowFinalize(&gw);
        }
        nvnTextureFinalize(&wtex);
    }

    ok = ok && fence_ok && wait_ok && queue_wait_ok && map_ok && pattern_ok &&
         acquire_ok && acquire_sync_ok && window_layout_ok;
    fprintf(stderr, "[totk-nvn] fence probe submit=%llu fence=%d wait=%d queue-wait=%d"
                    " map=%d pattern=%d acquire=%d acquire-sync=%d win-layout=%d"
                    " center=%02x%02x%02x%02x ok=%d\n",
            (unsigned long long)q.submits, fence_ok, wait_ok, queue_wait_ok, map_ok,
            pattern_ok, acquire_ok, acquire_sync_ok, window_layout_ok,
            px[0], px[1], px[2], px[3], ok);

    nvnSyncFinalize(&fence_sync);
    nvnCommandBufferFinalize(&cb);
    nvnWindowFinalize(&w);
    nvnMemoryPoolFinalize(&pool);
    free(pool_mem);
    runtime_native_window_destroy(nw);
    return ok;
}

static uint32_t nvnSyncInitialize(sync_t *s, const nvn_dev_t *d) { nvn_api("nvnSyncInitialize");
    if (!s || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnSyncInitialize(device)")) return 0;
    memset(s, 0, sizeof *s);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C');
    return 1;
}
static void nvnSyncFinalize(sync_t *s) {
    if (!s) return;
    if (s->fence) gfx_backend_fence_destroy((GfxFence *)s->fence);
    s->fence = NULL;
    s->magic = 0;
}
static void nvnSyncSetDebugLabel(sync_t *s, const char *l) { (void)s; (void)l; }

/* Signal only after the queue reports completion: a fence is never reported
 * signaled ahead of the GPU work submitted before it. */
static int nvn_sync_complete(sync_t *s, const char *what) {
    if (!s || !s->fence) return 0;
    if (s->state) return 1;
    if (!nvn_gpu_drain(what)) return 0;
    gfx_backend_fence_signal((GfxFence *)s->fence);
    s->state = 1;
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] %s signaled after GPU completion (sync=%p)\n", what, (void *)s);
    return 1;
}

static int nvnQueueFenceSync(queue_t *q, sync_t *s, int condition, int flags) { nvn_api("nvnQueueFenceSync");
    (void)flags;
    if (!nvn_obj(q, NVN_MAGIC('Q', 'U', 'E', 'U'), "nvnQueueFenceSync(queue)")) return 0;
    if (!nvn_obj(s, NVN_MAGIC('S', 'Y', 'N', 'C'), "nvnQueueFenceSync(sync)")) return 0;
    if (s->fence) {
        gfx_backend_fence_destroy((GfxFence *)s->fence);
        s->fence = NULL;
    }
    s->state = 0;
    s->condition = condition;
    s->submit_generation = nvn_gpu_submits;
    s->fence = gfx_backend_fence_create();
    if (!s->fence) {
        nvn_fail("nvnQueueFenceSync", "backend fence creation failed");
        return 0;
    }
    s->fence_generation++;
    return nvn_sync_complete(s, "nvnQueueFenceSync");
}
static int nvnSyncWait(const sync_t *s, uint64_t timeout) {
    if (!s || s->magic != NVN_MAGIC('S', 'Y', 'N', 'C')) {
        nvn_fail("nvnSyncWait", "invalid sync");
        return NVN_SYNC_WAIT_FAILED;
    }
    if (s->state) return NVN_SYNC_WAIT_ALREADY_SIGNALED;
    if (!s->fence) return NVN_SYNC_WAIT_ALREADY_SIGNALED;  /* never fenced */
    int r = gfx_backend_fence_wait((GfxFence *)s->fence, timeout);
    if (r > 0) {
        ((sync_t *)s)->state = 1;   /* the guest owns the mutable sync object */
        return NVN_SYNC_WAIT_CONDITION_SATISFIED;
    }
    if (r == 0) return NVN_SYNC_WAIT_TIMEOUT_EXPIRED;
    return NVN_SYNC_WAIT_FAILED;
}
static uint32_t nvnSyncInitializeFromFencedGLSync(sync_t *s, const nvn_dev_t *d, const void *gl) {
    (void)gl;
    return nvnSyncInitialize(s, d);
}
static void *nvnSyncCreateGLSync(const sync_t *s) { (void)s; return NULL; }

static void nvnEventBuilderSetDefaults(ev_builder_t *b) { if (b) memset(b, 0, sizeof *b); }
static void nvnEventBuilderSetStorage(ev_builder_t *b, const pool_t *p, int64_t off) {
    if (b) { b->pool = (pool_t *)p; b->off = off; }
}
static void nvnEventBuilderGetStorage(const ev_builder_t *b, void **pool, int64_t *off) {
    if (pool) *pool = b ? b->pool : NULL;
    if (off) *off = b ? b->off : 0;
}
static void *nvnEventBuilderGetMemoryPool(const ev_builder_t *b) { return b ? b->pool : NULL; }
static int64_t nvnEventBuilderGetMemoryOffset(const ev_builder_t *b) { return b ? b->off : 0; }
static uint32_t nvnEventInitialize(ev_t *e, const ev_builder_t *b) { nvn_api("nvnEventInitialize");
    if (!e || !b) { nvn_fail("nvnEventInitialize", "null event/builder"); return 0; }
    if (!nvn_obj(b->pool, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnEventInitialize(pool)")) return 0;
    if (b->off < 0 || (uint64_t)b->off + 4 > b->pool->size || !b->pool->mem) {
        nvn_fail("nvnEventInitialize", "event storage is not mapped");
        return 0;
    }
    memset(e, 0, sizeof *e);
    e->magic = NVN_MAGIC('E', 'V', 'N', 'T');
    e->pool = b->pool;
    e->off = b->off;
    e->val = (volatile uint32_t *)(b->pool->mem + b->off);
    return 1;
}
static void nvnEventFinalize(ev_t *e) { if (e) e->magic = 0; }
static uint32_t nvnEventGetValue(const ev_t *e) {
    if (!e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T') || !e->val) return 0;
    return *e->val;
}
static void nvnEventSignal(ev_t *e, int mode, uint32_t v) {
    if (!e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T') || !e->val) {
        nvn_fail("nvnEventSignal", "invalid event");
        return;
    }
    if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v;
    else *e->val = v;
}
static void *nvnEventGetMemoryPool(const ev_t *e) { return e ? e->pool : NULL; }
static int64_t nvnEventGetMemoryOffset(const ev_t *e) { return e ? e->off : 0; }

/* ------------------------------------------------------------------ */
/* Proc-address table                                                  */

typedef struct { const char *name; void (*fn)(void); } nvn_entry;

#define NVN_E(x) { #x, (void (*)(void))x }
static const nvn_entry nvn_entries[] = {
    NVN_E(nvnDeviceBuilderSetDefaults), NVN_E(nvnDeviceBuilderSetFlags),
    NVN_E(nvnDeviceBuilderGetFlags), NVN_E(nvnDeviceInitialize),
    NVN_E(nvnDeviceFinalize), NVN_E(nvnDeviceSetDebugLabel),
    NVN_E(nvnDeviceGetInteger), NVN_E(nvnDeviceGetCurrentTimestampInNanoseconds),
    NVN_E(nvnDeviceGetTimestampInNanoseconds), NVN_E(nvnDeviceSetIntermediateShaderCache),
    NVN_E(nvnDeviceGetTextureHandle), NVN_E(nvnDeviceGetTexelFetchHandle),
    NVN_E(nvnDeviceGetImageHandle), NVN_E(nvnDeviceInstallDebugCallback),
    NVN_E(nvnDeviceGenerateDebugDomainId), NVN_E(nvnDeviceSetWindowOriginMode),
    NVN_E(nvnDeviceSetDepthMode), NVN_E(nvnDeviceRegisterFastClearColor),
    NVN_E(nvnDeviceRegisterFastClearColori), NVN_E(nvnDeviceRegisterFastClearColorui),
    NVN_E(nvnDeviceRegisterFastClearDepth), NVN_E(nvnDeviceGetWindowOriginMode),
    NVN_E(nvnDeviceGetDepthMode), NVN_E(nvnDeviceApplyDeferredFinalizes),
    NVN_E(nvnDeviceFinalizeCommandHandle), NVN_E(nvnDeviceWalkDebugDatabase),
    NVN_E(nvnDeviceGetSeparateTextureHandle), NVN_E(nvnDeviceGetSeparateSamplerHandle),
    NVN_E(nvnDeviceIsExternalDebuggerAttached), NVN_E(nvnDeviceSetAutomaticCheckpointsEnable),
    NVN_E(nvnDeviceSetAutomaticCheckpointsMask), NVN_E(nvnDeviceSetAutomaticCheckpointsSamplingInterval),
    NVN_E(nvnDeviceSetAutomaticCheckpointsByDebugGroup), NVN_E(nvnDeviceSetCheckpointCallStacksEnable),
    NVN_E(nvnDeviceSetCheckpointMaxCallStacksDepth),

    NVN_E(nvnQueueBuilderSetDefaults), NVN_E(nvnQueueBuilderSetDevice),
    NVN_E(nvnQueueBuilderSetFlags), NVN_E(nvnQueueBuilderSetCommandMemorySize),
    NVN_E(nvnQueueBuilderSetComputeMemorySize), NVN_E(nvnQueueBuilderSetControlMemorySize),
    NVN_E(nvnQueueBuilderSetCommandFlushThreshold), NVN_E(nvnQueueBuilderSetQueuePriority),
    NVN_E(nvnQueueBuilderGetQueuePriority), NVN_E(nvnQueueBuilderGetQueueMemorySize),
    NVN_E(nvnQueueBuilderSetQueueMemory), NVN_E(nvnQueueBuilderGetMemory),
    NVN_E(nvnQueueBuilderGetMemorySize), NVN_E(nvnQueueBuilderGetDevice),
    NVN_E(nvnQueueBuilderGetFlags), NVN_E(nvnQueueBuilderGetCommandMemorySize),
    NVN_E(nvnQueueBuilderGetComputeMemorySize), NVN_E(nvnQueueBuilderGetControlMemorySize),
    NVN_E(nvnQueueBuilderGetCommandFlushThreshold), NVN_E(nvnQueueInitialize),
    NVN_E(nvnQueueFinalize), NVN_E(nvnQueueSetDebugLabel), NVN_E(nvnQueueSubmitCommands),
    NVN_E(nvnQueueFlush), NVN_E(nvnQueueFinish), NVN_E(nvnQueuePresentTexture),
    NVN_E(nvnQueueAcquireTexture), NVN_E(nvnQueueFenceSync), NVN_E(nvnQueueWaitSync),
    NVN_E(nvnQueueGetError), NVN_E(nvnQueueGetTotalCommandMemoryUsed),
    NVN_E(nvnQueueGetTotalControlMemoryUsed), NVN_E(nvnQueueGetTotalComputeMemoryUsed),
    NVN_E(nvnQueueResetMemoryUsageCounts),

    NVN_E(nvnWindowBuilderSetDefaults), NVN_E(nvnWindowBuilderSetDevice),
    NVN_E(nvnWindowBuilderSetNativeWindow), NVN_E(nvnWindowBuilderSetTextures),
    NVN_E(nvnWindowBuilderSetPresentInterval), NVN_E(nvnWindowBuilderSetNumActiveTextures),
    NVN_E(nvnWindowBuilderGetNativeWindow), NVN_E(nvnWindowBuilderGetPresentInterval),
    NVN_E(nvnWindowBuilderGetNumTextures), NVN_E(nvnWindowBuilderGetTexture),
    NVN_E(nvnWindowBuilderGetDevice), NVN_E(nvnWindowBuilderGetNumActiveTextures),
    NVN_E(nvnWindowInitialize), NVN_E(nvnWindowFinalize), NVN_E(nvnWindowSetDebugLabel),
    NVN_E(nvnWindowAcquireTexture), NVN_E(nvnWindowGetNativeWindow),
    NVN_E(nvnWindowGetPresentInterval), NVN_E(nvnWindowSetPresentInterval),
    NVN_E(nvnWindowSetCrop), NVN_E(nvnWindowGetCrop),
    NVN_E(nvnWindowSetNumActiveTextures), NVN_E(nvnWindowGetNumActiveTextures),
    NVN_E(nvnWindowGetNumTextures),

    NVN_E(nvnMemoryPoolBuilderSetDefaults), NVN_E(nvnMemoryPoolBuilderSetDevice),
    NVN_E(nvnMemoryPoolBuilderSetStorage), NVN_E(nvnMemoryPoolBuilderSetFlags),
    NVN_E(nvnMemoryPoolBuilderGetMemory), NVN_E(nvnMemoryPoolBuilderGetSize),
    NVN_E(nvnMemoryPoolBuilderGetFlags), NVN_E(nvnMemoryPoolBuilderGetDevice),
    NVN_E(nvnMemoryPoolInitialize), NVN_E(nvnMemoryPoolFinalize),
    NVN_E(nvnMemoryPoolSetDebugLabel), NVN_E(nvnMemoryPoolMap),
    NVN_E(nvnMemoryPoolFlushMappedRange), NVN_E(nvnMemoryPoolInvalidateMappedRange),
    NVN_E(nvnMemoryPoolGetBufferAddress), NVN_E(nvnMemoryPoolMapVirtual),
    NVN_E(nvnMemoryPoolGetSize), NVN_E(nvnMemoryPoolGetFlags),

    NVN_E(nvnBufferBuilderSetDefaults), NVN_E(nvnBufferBuilderSetDevice),
    NVN_E(nvnBufferBuilderSetStorage), NVN_E(nvnBufferBuilderGetDevice),
    NVN_E(nvnBufferBuilderGetMemoryPool), NVN_E(nvnBufferBuilderGetMemoryOffset),
    NVN_E(nvnBufferBuilderGetSize), NVN_E(nvnBufferInitialize), NVN_E(nvnBufferFinalize),
    NVN_E(nvnBufferSetDebugLabel), NVN_E(nvnBufferMap), NVN_E(nvnBufferGetAddress),
    NVN_E(nvnBufferFlushMappedRange), NVN_E(nvnBufferInvalidateMappedRange),
    NVN_E(nvnBufferGetMemoryPool), NVN_E(nvnBufferGetMemoryOffset),
    NVN_E(nvnBufferGetSize), NVN_E(nvnBufferGetDebugID),

    NVN_E(nvnTextureBuilderSetDefaults), NVN_E(nvnTextureBuilderSetDevice),
    NVN_E(nvnTextureBuilderSetFlags), NVN_E(nvnTextureBuilderSetTarget),
    NVN_E(nvnTextureBuilderSetWidth), NVN_E(nvnTextureBuilderSetHeight),
    NVN_E(nvnTextureBuilderSetDepth), NVN_E(nvnTextureBuilderSetSize1D),
    NVN_E(nvnTextureBuilderSetSize2D), NVN_E(nvnTextureBuilderSetSize3D),
    NVN_E(nvnTextureBuilderSetLevels), NVN_E(nvnTextureBuilderSetFormat),
    NVN_E(nvnTextureBuilderSetSamples), NVN_E(nvnTextureBuilderSetSwizzle),
    NVN_E(nvnTextureBuilderSetDepthStencilMode), NVN_E(nvnTextureBuilderSetStorage),
    NVN_E(nvnTextureBuilderSetPackagedTextureData), NVN_E(nvnTextureBuilderSetPackagedTextureLayout),
    NVN_E(nvnTextureBuilderSetStride), NVN_E(nvnTextureBuilderSetGLTextureName),
    NVN_E(nvnTextureBuilderGetStorageSize), NVN_E(nvnTextureBuilderGetStorageAlignment),
    NVN_E(nvnTextureBuilderGetStorageClass), NVN_E(nvnTextureBuilderGetViewOffset),
    NVN_E(nvnTextureBuilderGetPackagedTextureLayout), NVN_E(nvnTextureBuilderGetRawStorageClass),
    NVN_E(nvnTextureBuilderGetFlags),
    NVN_E(nvnTextureBuilderGetTarget), NVN_E(nvnTextureBuilderGetWidth),
    NVN_E(nvnTextureBuilderGetHeight), NVN_E(nvnTextureBuilderGetDepth),
    NVN_E(nvnTextureBuilderGetLevels), NVN_E(nvnTextureBuilderGetFormat),
    NVN_E(nvnTextureBuilderGetSamples), NVN_E(nvnTextureBuilderGetSwizzle),
    NVN_E(nvnTextureBuilderGetDepthStencilMode), NVN_E(nvnTextureBuilderGetPackagedTextureData),
    NVN_E(nvnTextureBuilderGetStride), NVN_E(nvnTextureBuilderGetSparseTileLayout),
    NVN_E(nvnTextureBuilderGetGLTextureName), NVN_E(nvnTextureBuilderGetZCullStorageSize),
    NVN_E(nvnTextureBuilderGetMemoryPool), NVN_E(nvnTextureBuilderGetMemoryOffset),
    NVN_E(nvnTextureBuilderGetDevice),
    NVN_E(nvnTextureInitialize), NVN_E(nvnTextureFinalize), NVN_E(nvnTextureSetDebugLabel),
    NVN_E(nvnTextureGetStorageClass), NVN_E(nvnTextureGetViewOffset), NVN_E(nvnTextureGetFlags),
    NVN_E(nvnTextureGetTarget), NVN_E(nvnTextureGetWidth), NVN_E(nvnTextureGetHeight),
    NVN_E(nvnTextureGetDepth), NVN_E(nvnTextureGetLevels), NVN_E(nvnTextureGetFormat),
    NVN_E(nvnTextureGetSamples), NVN_E(nvnTextureGetSwizzle), NVN_E(nvnTextureGetDepthStencilMode),
    NVN_E(nvnTextureGetStride), NVN_E(nvnTextureGetTextureAddress),
    NVN_E(nvnTextureGetSparseTileLayout), NVN_E(nvnTextureWriteTexels),
    NVN_E(nvnTextureWriteTexelsStrided), NVN_E(nvnTextureReadTexels),
    NVN_E(nvnTextureReadTexelsStrided), NVN_E(nvnTextureFlushTexels),
    NVN_E(nvnTextureInvalidateTexels), NVN_E(nvnTextureGetMemoryPool),
    NVN_E(nvnTextureGetMemoryOffset), NVN_E(nvnTextureGetStorageSize),
    NVN_E(nvnTextureCompare), NVN_E(nvnTextureGetDebugID), NVN_E(nvnTextureGetRawStorageClass),
    NVN_E(nvnTextureGetZCullStorageSize),

    NVN_E(nvnTextureViewSetDefaults), NVN_E(nvnTextureViewSetLevels),
    NVN_E(nvnTextureViewSetLayers), NVN_E(nvnTextureViewSetFormat),
    NVN_E(nvnTextureViewSetSwizzle), NVN_E(nvnTextureViewSetDepthStencilMode),
    NVN_E(nvnTextureViewSetTarget), NVN_E(nvnTextureViewGetLevels),
    NVN_E(nvnTextureViewGetLayers), NVN_E(nvnTextureViewGetFormat),
    NVN_E(nvnTextureViewGetSwizzle), NVN_E(nvnTextureViewGetDepthStencilMode),
    NVN_E(nvnTextureViewGetTarget), NVN_E(nvnTextureViewCompare),

    NVN_E(nvnSamplerBuilderSetDefaults), NVN_E(nvnSamplerBuilderSetDevice),
    NVN_E(nvnSamplerBuilderSetMinMagFilter), NVN_E(nvnSamplerBuilderSetWrapMode),
    NVN_E(nvnSamplerBuilderSetLodClamp), NVN_E(nvnSamplerBuilderSetLodBias),
    NVN_E(nvnSamplerBuilderSetCompare), NVN_E(nvnSamplerBuilderSetBorderColor),
    NVN_E(nvnSamplerBuilderSetBorderColori), NVN_E(nvnSamplerBuilderSetBorderColorui),
    NVN_E(nvnSamplerBuilderSetMaxAnisotropy), NVN_E(nvnSamplerBuilderSetReductionFilter),
    NVN_E(nvnSamplerBuilderSetLodSnap), NVN_E(nvnSamplerBuilderGetDevice),
    NVN_E(nvnSamplerBuilderGetMinMagFilter), NVN_E(nvnSamplerBuilderGetWrapMode),
    NVN_E(nvnSamplerBuilderGetLodClamp), NVN_E(nvnSamplerBuilderGetLodBias),
    NVN_E(nvnSamplerBuilderGetCompare), NVN_E(nvnSamplerBuilderGetBorderColor),
    NVN_E(nvnSamplerBuilderGetBorderColori), NVN_E(nvnSamplerBuilderGetBorderColorui),
    NVN_E(nvnSamplerBuilderGetMaxAnisotropy), NVN_E(nvnSamplerBuilderGetReductionFilter),
    NVN_E(nvnSamplerBuilderGetLodSnap),
    NVN_E(nvnSamplerInitialize), NVN_E(nvnSamplerFinalize), NVN_E(nvnSamplerSetDebugLabel),
    NVN_E(nvnSamplerGetMinMagFilter), NVN_E(nvnSamplerGetWrapMode),
    NVN_E(nvnSamplerGetLodClamp), NVN_E(nvnSamplerGetLodBias),
    NVN_E(nvnSamplerGetCompare), NVN_E(nvnSamplerGetBorderColor),
    NVN_E(nvnSamplerGetBorderColori), NVN_E(nvnSamplerGetBorderColorui),
    NVN_E(nvnSamplerGetMaxAnisotropy), NVN_E(nvnSamplerGetReductionFilter),
    NVN_E(nvnSamplerCompare), NVN_E(nvnSamplerGetDebugID),

    NVN_E(nvnTexturePoolInitialize), NVN_E(nvnTexturePoolFinalize),
    NVN_E(nvnTexturePoolSetDebugLabel), NVN_E(nvnTexturePoolRegisterTexture),
    NVN_E(nvnTexturePoolRegisterImage), NVN_E(nvnTexturePoolGetMemoryPool),
    NVN_E(nvnTexturePoolGetMemoryOffset), NVN_E(nvnTexturePoolGetSize),
    NVN_E(nvnSamplerPoolInitialize), NVN_E(nvnSamplerPoolFinalize),
    NVN_E(nvnSamplerPoolSetDebugLabel), NVN_E(nvnSamplerPoolRegisterSampler),
    NVN_E(nvnSamplerPoolRegisterSamplerBuilder), NVN_E(nvnSamplerPoolGetMemoryPool),
    NVN_E(nvnSamplerPoolGetMemoryOffset), NVN_E(nvnSamplerPoolGetSize),

    NVN_E(nvnCommandBufferInitialize), NVN_E(nvnCommandBufferFinalize),
    NVN_E(nvnCommandBufferSetDebugLabel), NVN_E(nvnCommandBufferSetMemoryCallback),
    NVN_E(nvnCommandBufferSetMemoryCallbackData), NVN_E(nvnCommandBufferSetControlMemoryCallbackEnabled),
    NVN_E(nvnCommandBufferSetCommandMemoryCallbackEnabled), NVN_E(nvnCommandBufferAddCommandMemory),
    NVN_E(nvnCommandBufferAddControlMemory), NVN_E(nvnCommandBufferGetControlMemorySize),
    NVN_E(nvnCommandBufferGetControlMemoryUsed), NVN_E(nvnCommandBufferGetCommandMemorySize),
    NVN_E(nvnCommandBufferGetCommandMemoryUsed), NVN_E(nvnCommandBufferBeginRecording),
    NVN_E(nvnCommandBufferEndRecording), NVN_E(nvnCommandBufferSetRenderEnable),
    NVN_E(nvnCommandBufferSetRenderTargets), NVN_E(nvnCommandBufferClearColor),
    NVN_E(nvnCommandBufferClearColori), NVN_E(nvnCommandBufferClearColorui),
    NVN_E(nvnCommandBufferDrawArrays), NVN_E(nvnCommandBufferDrawArraysInstanced),
    NVN_E(nvnCommandBufferDrawElements), NVN_E(nvnCommandBufferDrawElementsBaseVertex), NVN_E(nvnCommandBufferClearDepthStencil),
    NVN_E(nvnCommandBufferSetViewport), NVN_E(nvnCommandBufferSetViewports),
    NVN_E(nvnCommandBufferSetScissor), NVN_E(nvnCommandBufferSetScissors),
    NVN_E(nvnCommandBufferBindProgram), NVN_E(nvnCommandBufferBindVertexBuffer),
    NVN_E(nvnCommandBufferBindVertexBuffers), NVN_E(nvnCommandBufferBindVertexAttribState),
    NVN_E(nvnCommandBufferBindVertexStreamState), NVN_E(nvnCommandBufferSetPrimitiveRestart),
    NVN_E(nvnCommandBufferBindBlendState), NVN_E(nvnCommandBufferBindColorState),
    NVN_E(nvnCommandBufferBindChannelMaskState), NVN_E(nvnCommandBufferBindMultisampleState),
    NVN_E(nvnCommandBufferBindPolygonState), NVN_E(nvnCommandBufferBindDepthStencilState),
    NVN_E(nvnCommandBufferSetBlendColor), NVN_E(nvnCommandBufferSetStencilRef),
    NVN_E(nvnCommandBufferSetStencilMask), NVN_E(nvnCommandBufferSetStencilValueMask),
    NVN_E(nvnCommandBufferSetTexturePool),
    NVN_E(nvnCommandBufferSetSamplerPool), NVN_E(nvnCommandBufferBindUniformBuffer),
    NVN_E(nvnCommandBufferBindUniformBuffers), NVN_E(nvnCommandBufferBindStorageBuffer),
    NVN_E(nvnCommandBufferBindStorageBuffers), NVN_E(nvnCommandBufferBindTexture),
    NVN_E(nvnCommandBufferBindTextures), NVN_E(nvnCommandBufferBindImage),
    NVN_E(nvnCommandBufferBindImages),
    NVN_E(nvnBlendStateSetDefaults), NVN_E(nvnBlendStateSetBlendTarget),
    NVN_E(nvnBlendStateSetBlendFunc), NVN_E(nvnBlendStateSetBlendEquation),
    NVN_E(nvnColorStateSetDefaults), NVN_E(nvnColorStateSetBlendEnable),
    NVN_E(nvnChannelMaskStateSetDefaults), NVN_E(nvnChannelMaskStateSetChannelMask),
    NVN_E(nvnMultisampleStateSetDefaults), NVN_E(nvnMultisampleStateSetMultisampleEnable),
    NVN_E(nvnMultisampleStateSetSamples), NVN_E(nvnMultisampleStateSetAlphaToCoverageEnable),
    NVN_E(nvnMultisampleStateSetSampleLocationsEnable),
    NVN_E(nvnMultisampleStateSetSampleLocations),
    NVN_E(nvnPolygonStateSetDefaults), NVN_E(nvnPolygonStateSetCullFace),
    NVN_E(nvnPolygonStateSetFrontFace), NVN_E(nvnPolygonStateSetPolygonMode),
    NVN_E(nvnPolygonStateSetPolygonOffsetEnables),
    NVN_E(nvnDepthStencilStateSetDefaults), NVN_E(nvnDepthStencilStateSetDepthTestEnable),
    NVN_E(nvnDepthStencilStateSetDepthWriteEnable), NVN_E(nvnDepthStencilStateSetDepthFunc),
    NVN_E(nvnDepthStencilStateSetStencilTestEnable), NVN_E(nvnDepthStencilStateSetStencilFunc),
    NVN_E(nvnDepthStencilStateSetStencilOp),
    NVN_E(nvnProgramInitialize), NVN_E(nvnProgramFinalize), NVN_E(nvnProgramSetDebugLabel),
    NVN_E(nvnProgramSetShaders), NVN_E(nvnProgramSetShadersExt),
    NVN_E(nvnProgramSetSampleShading), NVN_E(nvnProgramGetSubtileSize),
    NVN_E(nvnProgramSetSubroutineLinkage),

    NVN_E(nvnSyncInitialize), NVN_E(nvnSyncFinalize), NVN_E(nvnSyncSetDebugLabel),
    NVN_E(nvnSyncWait), NVN_E(nvnSyncInitializeFromFencedGLSync), NVN_E(nvnSyncCreateGLSync),
    NVN_E(nvnEventBuilderSetDefaults), NVN_E(nvnEventBuilderSetStorage),
    NVN_E(nvnEventBuilderGetStorage), NVN_E(nvnEventBuilderGetMemoryPool),
    NVN_E(nvnEventBuilderGetMemoryOffset), NVN_E(nvnEventInitialize),
    NVN_E(nvnEventFinalize), NVN_E(nvnEventGetValue), NVN_E(nvnEventSignal),
    NVN_E(nvnEventGetMemoryPool), NVN_E(nvnEventGetMemoryOffset),
};
#undef NVN_E

static uint64_t nvn_zero64(void) { return 0; }

static void *nvn_lookup(const char *name) {
    if (!name) return NULL;
    snprintf(g_last_proc, sizeof g_last_proc, "%s", name);
    for (unsigned i = 0; i < sizeof nvn_entries / sizeof *nvn_entries; i++)
        if (!strcmp(name, nvn_entries[i].name)) return (void *)nvn_entries[i].fn;
    if (nvn_trace()) fprintf(stderr, "[totk-nvn] proc lookup %s (unimplemented)\n", name);
    /* Object APIs must not silently report success.  The object categories
     * below have complete implementations, so an unknown name here is a real
     * gap: report it and return a zero-returning entry. */
    static const char *const object_prefixes[] = {
        "nvnDevice", "nvnQueue", "nvnWindow", "nvnMemoryPool", "nvnBuffer",
        "nvnTexture", "nvnSampler", "nvnProgram", "nvnSync", "nvnEvent",
    };
    for (unsigned i = 0; i < sizeof object_prefixes / sizeof *object_prefixes; i++)
        if (!strncmp(name, object_prefixes[i], strlen(object_prefixes[i])))
            nvn_fail("unimplemented NVN object API", name);
    return (void *)nvn_zero64;
}

static void *nvnDeviceGetProcAddress(const nvn_dev_t *d, const char *name) {
    nvn_api("nvnDeviceGetProcAddress");
    (void)d;
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvnDeviceGetProcAddress;
    return nvn_lookup(name);
}

void *runtime_nvn_bootstrap(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceInitialize")) return (void *)nvnDeviceInitialize;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvnDeviceGetProcAddress;
    return nvn_lookup(name);
}
