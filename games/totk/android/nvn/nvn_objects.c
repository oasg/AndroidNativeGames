/* NVN module: objects. */
#include "nvn_internal.h"

/* Resolve the NVN texture set to a host layout.  Every color target and the
 * depth target contribute their guest dimensions/format; unsupported formats
 * are explicit failures (the backend returns VK_FORMAT_UNDEFINED = 0). */
/* NVNtextureView (16 bytes): flag byte 0 (bit0 levels, bit1 layers, bit2
 * format, bit3 target, bit4 swizzle, bit5 depth-stencil mode), byte1 base
 * level, byte2 level count, byte3 target, u16 min layer, u16 layer count,
 * u16 format at offset 8.  A render target may be attached through such a
 * view, so the attachment format can differ from the base texture's format
 * (e.g. a BC4 atlas rendered through an R8 view). */
int nvn_view_format(const void *view, int *format, uint32_t *base_layer,
                           uint32_t *layers) {
    const uint8_t *v = view;
    if (!v) return 0;
    int have = 0;
    if (v[0] & 4) {
        int f = (int)((uint32_t)v[8] | ((uint32_t)v[9] << 8));
        if (f) { *format = f; have = 1; }
    }
    if (v[0] & 2) {
        if (base_layer) *base_layer = (uint32_t)v[4] | ((uint32_t)v[5] << 8);
        if (layers) *layers = (uint32_t)v[6] | ((uint32_t)v[7] << 8);
    }
    return have;
}

void nvnBlendStateSetDefaults(void *s) {
    if (s) *(nvn_blend_st *)s = (nvn_blend_st){ 0, 2, 1, 2, 1, 1, 1 };
}

void nvnBlendStateSetBlendTarget(void *s, int t) { if (s) ((nvn_blend_st *)s)->target = (uint32_t)t; }

void nvnBlendStateSetBlendFunc(void *s, int sr, int dr, int sa, int da) {
    if (!s) return;
    nvn_blend_st *o = s;
    o->src_rgb = (uint32_t)sr; o->dst_rgb = (uint32_t)dr;
    o->src_a = (uint32_t)sa; o->dst_a = (uint32_t)da;
}

void nvnBlendStateSetBlendEquation(void *s, int er, int ea) {
    if (!s) return;
    ((nvn_blend_st *)s)->eq_rgb = (uint32_t)er;
    ((nvn_blend_st *)s)->eq_a = (uint32_t)ea;
}

void nvnColorStateSetDefaults(void *s) {
    if (s) *(nvn_color_st *)s = (nvn_color_st){ 0, 3, 8 };
}

void nvnColorStateSetBlendEnable(void *s, int i, uint32_t en) {
    if (!s || i < 0 || i > 7) return;
    nvn_color_st *o = s;
    if (en) o->blend_en |= 1u << i; else o->blend_en &= ~(1u << i);
}

void nvnChannelMaskStateSetDefaults(void *s) { if (s) ((nvn_chmask_st *)s)->mask = 0xffffffffu; }

void nvnChannelMaskStateSetChannelMask(void *s, int i, uint32_t r, uint32_t g,
                                              uint32_t b, uint32_t a) {
    if (!s || i < 0 || i > 7) return;
    uint32_t m = (r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u);
    nvn_chmask_st *o = s;
    o->mask = (o->mask & ~(0xfu << (4 * i))) | (m << (4 * i));
}

void nvnMultisampleStateSetDefaults(void *s) {
    if (!s) return;
    memset(s, 0, sizeof(nvn_ms_st));
    ((nvn_ms_st *)s)->enable = 1;
}

void nvnMultisampleStateSetMultisampleEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->enable = e != 0;
}

void nvnMultisampleStateSetSamples(void *s, int n) {
    if (s) ((nvn_ms_st *)s)->samples = (uint8_t)n;
}

void nvnMultisampleStateSetAlphaToCoverageEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->a2c = e != 0;
}

void nvnMultisampleStateSetSampleLocationsEnable(void *s, uint32_t e) {
    if (s) ((nvn_ms_st *)s)->sl_en = e != 0;
}

void nvnMultisampleStateSetSampleLocations(void *s, int start, int n, const float *v) {
    (void)s; (void)start; (void)n; (void)v;
}

void nvnPolygonStateSetDefaults(void *s) {
    if (s) *(nvn_poly_st *)s = (nvn_poly_st){ 0, 1, 2, 0 };
}

void nvnPolygonStateSetCullFace(void *s, int f) { if (s) ((nvn_poly_st *)s)->cull = (uint32_t)f; }

void nvnPolygonStateSetFrontFace(void *s, int f) { if (s) ((nvn_poly_st *)s)->front = (uint32_t)f; }

void nvnPolygonStateSetPolygonMode(void *s, int m) { if (s) ((nvn_poly_st *)s)->mode = (uint32_t)m; }

void nvnPolygonStateSetPolygonOffsetEnables(void *s, int e) {
    if (s) ((nvn_poly_st *)s)->offset_en = (uint32_t)e;
}

void nvnDepthStencilStateSetDefaults(void *s) {
    if (!s) return;
    *(nvn_ds_st *)s = (nvn_ds_st){ .dt = 0, .dw = 1, .df = 2, .st = 0,
        .sf_f = 8, .sf_b = 8, .fail_f = 1, .zfail_f = 1, .zpass_f = 1,
        .fail_b = 1, .zfail_b = 1, .zpass_b = 1 };
}

void nvnDepthStencilStateSetDepthTestEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->dt = e != 0; }

void nvnDepthStencilStateSetDepthWriteEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->dw = e != 0; }

void nvnDepthStencilStateSetDepthFunc(void *s, int f) { if (s) ((nvn_ds_st *)s)->df = (uint32_t)f; }

void nvnDepthStencilStateSetStencilTestEnable(void *s, uint32_t e) { if (s) ((nvn_ds_st *)s)->st = e != 0; }

void nvnDepthStencilStateSetStencilFunc(void *s, int f, int fn) {
    if (!s) return;
    nvn_ds_st *o = s;
    if (f & 1) o->sf_f = (uint32_t)fn;
    if (f & 2) o->sf_b = (uint32_t)fn;
}

void nvnDepthStencilStateSetStencilOp(void *s, int f, int fail, int zfail, int zpass) {
    if (!s) return;
    nvn_ds_st *o = s;
    if (f & 1) { o->fail_f = (uint32_t)fail; o->zfail_f = (uint32_t)zfail; o->zpass_f = (uint32_t)zpass; }
    if (f & 2) { o->fail_b = (uint32_t)fail; o->zfail_b = (uint32_t)zfail; o->zpass_b = (uint32_t)zpass; }
}
