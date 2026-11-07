/* NVN object/state layouts. */
#ifndef TOTK_ANDROID_NVN_TYPES_0_H
#define TOTK_ANDROID_NVN_TYPES_0_H
#include "nvn_prelude.h"

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
/* Repository-owned compute SPIR-V used by probe-nvn-compute (read-only). */

typedef uint32_t NvnBool;


typedef void *NvnNativeWindow;




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
    NVN_CMD_UPDATE_UBO,
    /* GPU transfer / compute commands (Wave 1 Lane 2).  Their payload is a
     * snapshot taken while the guest records, because a guest ring buffer can
     * be recycled long before the submit replays the command. */
    NVN_CMD_CLEAR_TEXTURE,
    NVN_CMD_COPY_TEXTURE_TO_TEXTURE,
    NVN_CMD_COPY_BUFFER_TO_TEXTURE,
    NVN_CMD_COPY_TEXTURE_TO_BUFFER,
    NVN_CMD_DISPATCH,
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



/* Host-side shader state of one NVNprogram (defined further down). */
typedef struct prog_shader_state prog_shader_state_t;



typedef struct {
    uint32_t kind;
    union {
        struct { float rgba[4]; int index; uint32_t mask; } clear;
        struct { float depth; uint32_t dmask; int stencil; uint32_t smask; } clear_ds;
        struct { int32_t x, y, width, height; } rect;
        /* Complete NVN render-target set: every color target survives to the
         * submit path, plus the depth target.  The resolved layout (view
         * formats included) is captured here so a segment replay does not
         * have to re-resolve a texture the guest may have re-created. */
        struct {
            uint32_t count;
            void *tex[GFX_MAX_COLOR_TARGETS];
            void *depth;
            GfxRenderTargetLayout layout;
            uint32_t width, height;
            int have_layout;
        } targets;
        struct { uint32_t stage, kind, slot; uint64_t address; size_t size; uint64_t handle; } desc;
        struct { nvn_render_state_t rs; } state;
        struct { uint32_t primitive, first, count, instances, base_instance;
                 /* Private copy of each bound vertex stream taken at record
                  * time.  The guest recycles its vertex ring before a queued
                  * segment replays, so the address alone is not enough. */
                 uint8_t *vb_data[4];
                 size_t vb_size[4];
                 uint8_t vb_stream[4];
                 uint32_t vb_count; } draw;
        struct {
            uint32_t primitive, count, index_type, base_vertex;
            uint64_t indices;
            /* Private copy of the index bytes taken at record time: a guest
             * ring buffer is recycled long before a retired segment replays,
             * and the index range is not protected by the vertex/UBO copies. */
            uint8_t *index_data;
            size_t index_size;
            /* Same per-draw vertex-stream snapshots as NVN_CMD_DRAW. */
            uint8_t *vb_data[4];
            size_t vb_size[4];
            uint8_t vb_stream[4];
            uint32_t vb_count;
        } draw_indexed;
        /* Copy of nvnCommandBufferUpdateUniformBuffer's payload; written to
         * dst at submit time, in recorded order (NVN semantics). */
        struct { uint64_t dst; uint32_t size; uint8_t *data; } update;
        /* ClearTexture: full payload snapshot (texture object, view level /
         * layer, texel region, colour and channel mask). */
        struct {
            void *tex;
            uint32_t level, layer;
            int32_t x, y;
            uint32_t width, height;
            float rgba[4];
            uint32_t mask;
            uint32_t integer;      /* ClearTexturei/ui: rgba holds raw int bits */
        } tex_clear;
        /* CopyTextureToTexture: both ends, both views and both regions. */
        struct {
            void *src, *dst;
            uint32_t src_level, src_layer, dst_level, dst_layer;
            int32_t sx, sy, sz, dx, dy, dz;
            uint32_t width, height, depth;
            uint32_t flags;
        } tex_copy;
        /* CopyBufferToTexture.  `data` is a private copy of the guest buffer
         * bytes taken at record time, so a recycled ring buffer cannot
         * corrupt the replay. */
        struct {
            void *tex;
            uint32_t level, layer;
            int32_t x, y;
            uint32_t width, height;
            uint64_t address;      /* guest buffer address (for evidence) */
            size_t size;           /* guest buffer size */
            uint8_t *data;
            size_t data_size;
            uint32_t flags;
        } buf_to_tex;
        /* CopyTextureToBuffer: read back into the guest buffer at submit. */
        struct {
            void *tex;
            uint32_t level, layer;
            int32_t x, y;
            uint32_t width, height;
            uint64_t address;
            size_t size;
            uint32_t flags;
        } tex_to_buf;
        /* DispatchCompute: the program and the descriptor snapshot are taken
         * while recording, never from the submit-time final state. */
        struct {
            uint32_t x, y, z;
            void *prog;
            struct nvn_dispatch_snap *bind;   /* private snapshot */
            prog_shader_state_t *pstate;      /* retained program state */
        } dispatch;
    } u;
} nvn_cmd_t;



/* Last bound descriptor state of a command buffer.  NVN binds by stage/slot;
 * the reflected pipeline maps that to a SPIR-V binding through .res.json. */
typedef struct nvn_bind_state {
    struct { uint64_t address; size_t size; } ubo[6][16];
    struct { uint64_t address; size_t size; } ssbo[6][16];
    uint64_t tex[6][128];
    /* Storage-image handles (nvnCommandBufferBindImage).  NVN's compute path
     * binds images at the same compact indices as textures, so keep the same
     * shape and mask convention. */
    uint64_t img[6][16];
    uint32_t ubo_mask[6], ssbo_mask[6];
    uint32_t tex_mask[6][4];
    uint32_t img_mask[6];
    uint32_t va[16];
    uint64_t vs[16];
    struct { uint64_t address; size_t size; } vb[16];
    uint32_t va_mask, vs_mask, vb_mask;
    nvn_render_state_t rs;
    const void *depth_tex;
} nvn_bind_state_t;



/* A dispatch's immutable descriptor snapshot.  Texture handles are resolved to
 * guest NVN texture objects while the texture pool is still live, so the
 * submit path never has to dereference a pool that the guest recycled. */
typedef struct nvn_dispatch_snap {
    nvn_bind_state_t state;
    const void *img_tex[16];
    const void *tex_tex[128];
} nvn_dispatch_snap_t;



/* Last uniform buffer bound by any command buffer, per (stage, slot).  NVN
 * state is queue-scoped on hardware: a bind recorded on one command buffer is
 * still live for a later submission unless another bind replaced it.  The
 * per-CB state is merged over this snapshot at submit time. */
typedef struct {
    uint64_t address;
    size_t size;
    uint32_t valid;
} nvn_global_ubo_t;



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



/* Must match the guest/macOS dpool layout for the first 0x18 bytes:
 * {NVNmemoryPool *pool; int64_t off; int32_t n}.  Host bookkeeping lives in
 * the 8-byte tail so we never move the guest's pool/off/n fields. */
typedef struct {
    pool_t *pool;
    int64_t off;
    int32_t n;
    uint32_t magic;
    uint32_t gen, epoch;
} tex_pool_t;



/* Queue-scoped texture binds.  TOTK binds resources on a state-only command
 * buffer and the draw buffers rely on them staying live; the draw's own
 * descriptor snapshot often contains only the subset it changed.  Keep the
 * last resolved object per (stage, slot) so a segment can inherit the rest. */
typedef struct { uint64_t handle; const tex_t *tex; } nvn_global_tex_t;



/* Host bridge for the NVN texture path; kept out of tex_t so the guest's
 * NVN_TEXTURE_SIZE contract is unchanged.  `gfx` is the sampled GfxTexture;
 * `rt` is the host render target this texture owns as colour attachment 0.
 * The compositor's texture and the scene pass's render target are the same
 * guest texture, so the bridge is where the two halves meet. */
typedef struct {
    const tex_t *tex;
    void *gfx;
    GfxOffscreenTarget *rt;
    GfxRenderTargetLayout rt_layout;
    uint32_t rt_width, rt_height;
    uint64_t rt_frame, rt_uploaded;   /* render generation / uploaded generation */
    int rt_valid;
    /* The guest wrote texels through the CPU path since the host copy was
     * uploaded; the cached GfxTexture is stale until it is rebuilt. */
    int gfx_dirty;
    /* Direct guest writes (font atlas / glyph cache) bypass every NVN texel
     * API.  A sampled checksum over the guest storage detects those writes so
     * the cached host copy can be rebuilt. */
    uint64_t mem_sum;
    size_t mem_sum_size;
    int mem_sum_valid;
} tex_bridge_t;



typedef struct {
    uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction;
    float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap;
} samp_par;



typedef struct { void *dev; samp_par p; } samp_builder_t;


typedef struct { uint32_t magic; samp_par p; } samp_t;

#endif
