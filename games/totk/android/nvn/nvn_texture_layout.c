/* NVN module: texture_layout. */
#include "nvn_internal.h"

void nvn_render_state_defaults(nvn_render_state_t *r) {
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

nvn_fmt_t nvn_fmt_of(int f) {
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

uint32_t nvn_mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

uint32_t nvn_blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }

unsigned nvn_pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }

unsigned nvn_adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}

void nvn_shape(const tex_desc *d, unsigned *dpl, int *layered,
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

void nvn_samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2;
    else if (s == 4) *sx = *sy = 2;
    else if (s == 8) { *sx = 4; *sy = 2; }
}

int nvn_tex_linear(const tex_desc *d) { return (d->flags & 0x50) != 0; }

uint32_t nvn_linear_stride(const tex_desc *d) {
    if (d->stride) return d->stride;
    int bw, bh; int bpb = nvn_fmt_of(d->format).bpb; (void)bw; (void)bh;
    uint32_t align = (d->flags & 0x40) ? 128 : 32;
    return ((((d->width ? d->width : 1) + nvn_fmt_of(d->format).bw - 1) /
             nvn_fmt_of(d->format).bw) * bpb + align - 1) & ~(align - 1);
}

uint64_t nvn_level_offset(const tex_desc *d, unsigned level, unsigned dpl,
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

uint64_t nvn_layer_bytes(const tex_desc *d, unsigned dpl, int layered,
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

void nvn_host_tex_layout(const tex_desc *d, tex_layout *o) {
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

uint64_t nvn_host_tex_level_offset(const tex_desc *d, unsigned level) {
    nvn_fmt_t t = nvn_fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    nvn_shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || nvn_tex_linear(d) || d->target == NVN_TARGET_BUFFER) return 0;
    tex_layout L; nvn_host_tex_layout(d, &L);
    return nvn_level_offset(d, level, dpl, w, h, z, L.tileH, L.tileD, t);
}
