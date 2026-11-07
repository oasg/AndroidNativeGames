/* NVN module: vertex_0. */
#include "nvn_internal.h"

/* NVNvertexAttribState is 4 packed bytes {fmt:8, offset:19, stream:5} and
 * NVNvertexStreamState is {stride, divisor}.  The guest fills these through
 * the setter entry points below before handing them to
 * nvnCommandBufferBindVertexAttribState/BindVertexStreamState; without the
 * setters every submitted state stays zero and the vertex input disappears. */
void nvnVertexAttribStateSetDefaults(void *s) { if (s) *(uint32_t *)s = 0; }

void nvnVertexAttribStateSetFormat(void *s, int format, ptrdiff_t offset) {
    if (!s) return;
    if (offset < 0 || offset >= (1 << 19)) {
        char d[64]; snprintf(d, sizeof d, "offset %lld", (long long)offset);
        nvn_fail("nvnVertexAttribStateSetFormat", d);
        if (offset < 0) return;
    }
    uint32_t v = *(uint32_t *)s;
    v = (v & ~0xffu) | ((uint32_t)format & 0xffu);
    v = (v & ~(0x7ffffu << 8)) | ((uint32_t)offset & 0x7ffffu) << 8;
    *(uint32_t *)s = v;
    if (nvn_trace_first() && nvn_vaset_trace++ < 64)
        fprintf(stderr, "[totk-nvn] AttribStateSetFormat state=%p fmt=%#x off=%lld -> %#x\n",
                s, (unsigned)format, (long long)offset, v);
}

void nvnVertexAttribStateSetStreamIndex(void *s, int index) {
    if (!s) return;
    if (index < 0 || index > 31) { nvn_fail("nvnVertexAttribStateSetStreamIndex", "index out of range"); return; }
    uint32_t v = *(uint32_t *)s;
    v = (v & ~(0x1fu << 27)) | ((uint32_t)index & 0x1fu) << 27;
    *(uint32_t *)s = v;
}

void nvnVertexAttribStateGetFormat(const void *s, int *format, ptrdiff_t *offset) {
    uint32_t v = s ? *(const uint32_t *)s : 0;
    if (format) *format = (int)(v & 0xffu);
    if (offset) *offset = (ptrdiff_t)((v >> 8) & 0x7ffffu);
}

int nvnVertexAttribStateGetStreamIndex(const void *s) {
    return s ? (int)((*(const uint32_t *)s >> 27) & 0x1fu) : 0;
}

void nvnVertexStreamStateSetDefaults(void *s) { if (s) memset(s, 0, 8); }

void nvnVertexStreamStateSetStride(void *s, ptrdiff_t stride) {
    if (!s) return;
    if (stride < 0 || (uint64_t)stride > 0xffffffffull) {
        nvn_fail("nvnVertexStreamStateSetStride", "stride out of range");
        return;
    }
    memcpy(s, &stride, sizeof(uint32_t));
    if (nvn_trace_first() && nvn_vaset_trace++ < 64)
        fprintf(stderr, "[totk-nvn] StreamStateSetStride state=%p stride=%lld\n",
                s, (long long)stride);
}

void nvnVertexStreamStateSetDivisor(void *s, int divisor) {
    if (!s) return;
    uint32_t d = (uint32_t)divisor;
    memcpy((uint8_t *)s + 4, &d, sizeof d);
    if (nvn_trace_first() && nvn_vaset_trace++ < 64)
        fprintf(stderr, "[totk-nvn] StreamStateSetDivisor state=%p div=%d\n", s, divisor);
}

ptrdiff_t nvnVertexStreamStateGetStride(const void *s) {
    uint32_t v = 0;
    if (s) memcpy(&v, s, sizeof v);
    return (ptrdiff_t)v;
}

int nvnVertexStreamStateGetDivisor(const void *s) {
    uint32_t v = 0;
    if (s) memcpy(&v, (const uint8_t *)s + 4, sizeof v);
    return (int)v;
}

/* Create (or reuse) the host pipeline for a program whose VS/PS came from the
 * offline cache.  The target format is part of the identity: the same program
 * can be drawn to the handheld and docked windows. */
char nvn_attr_class(uint32_t f) {
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

uint64_t nvn_variant_sig(const char *v) {
    if (!v || !*v) return 0;
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        h ^= *p; h *= 1099511628211ull;
    }
    return h ? h : 1;
}

/* Bind-state suffix shared with the offline packer:
 *   a<16 chars: f/u/s/_>  [-t<slot>_<target>[d]]...
 * Only emitted when the cache actually contains <key>-*.spv.  The offline
 * translator must be run with the matching attr/tex answers. */
void nvn_variant_suffix(prog_t *p, const cb_t *cb, char *out, size_t cap) {
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
        const tex_pool_t *tpool = nvn_tex_pool_for(cb);
        const tex_t *t = tpool ? nvn_tex_pool_lookup(tpool, tid) : NULL;
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

uint64_t nvn_vertex_signature(const GfxVertexLayout *layout) {
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

nvn_vfmt_t nvn_vfmt_of(int f) {
    if (f >= 0x51 && f <= 0x68) {
        static const uint8_t comps[24] = {1,1,1,1,1,1,2,2,2,2,2,2,3,3,3,3,3,3,4,4,4,4,4,4};
        static const uint8_t bytes[24] = {1,1,2,2,4,4,1,1,2,2,4,4,1,1,2,2,4,4,1,1,2,2,4,4};
        int k = f - 0x51;
        return (nvn_vfmt_t){ comps[k], bytes[k], (uint8_t)(k & 1), 1 };
    }
    return (nvn_vfmt_t){ 0, 0, 0, 0 };
}

int nvn_float_vk_format(int comps) {
    switch (comps) {
    case 1: return gfx_backend_vk_format_for_nvn(0x0A);   /* R32F */
    case 2: return gfx_backend_vk_format_for_nvn(0x16);   /* RG32F */
    case 3: return gfx_backend_vk_format_for_nvn(0x22);   /* RGB32F */
    case 4: return gfx_backend_vk_format_for_nvn(0x2E);   /* RGBA32F */
    default: return 0;
    }
}

void nvn_build_vertex_layout(const nvn_bind_state_t *b, GfxVertexLayout *out, uint64_t *sig) {
    memset(out, 0, sizeof *out);
    *sig = 0;
    if (!b || !b->va_mask) return;
    uint32_t used_streams = 0;
    uint32_t valid_streams = 0;
    uint32_t conv = 0;
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
        nvn_vfmt_t vf = nvn_vfmt_of((int)fmt);
        if (vf.convert) {
            int vkfmt = nvn_float_vk_format(vf.comps);
            if (!vkfmt || out->nbindings >= 16 || out->nattributes >= 16) { conv++; continue; }
            uint32_t bind = 24u + conv++;
            GfxVertexAttribute *a = &out->attributes[out->nattributes++];
            a->location = (uint32_t)i;
            a->binding = bind;
            a->format = vkfmt;
            a->offset = 0;
            GfxVertexBinding *v = &out->bindings[out->nbindings++];
            v->binding = bind;
            v->stride = (uint32_t)vf.comps * 4u;
            v->input_rate = 0;
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

/* Vertex count an indexed draw really needs, and a safe clamp.  The guest may
 * bind a vertex buffer smaller than the index range (TOTK binds 32 bytes and
 * indexes four vertices).  When the range is provably inside a guest pool the
 * stream is extended from guest memory; otherwise the index values are clamped
 * to the bound range and the clamp is reported (never a silent overwrite). */
uint32_t nvn_fix_index_range(GfxCmd *cmds, uint32_t count,
                                    const nvn_bind_state_t *b, const char *where) {
    uint32_t need = 0;
    if (!cmds || !b) return 0;
    if (!(b->vb_mask & 1u) || !b->vb[0].address || !b->vb[0].size || !b->vs[0])
        return 0;
    uint64_t addr = b->vb[0].address;
    uint32_t stride = (uint32_t)b->vs[0];
    uint32_t have = (uint32_t)(b->vb[0].size / stride);
    for (uint32_t i = 0; i < count; i++) {
        GfxCmd *c = &cmds[i];
        if (c->kind != GFX_CMD_DRAW_INDEXED) continue;
        uint32_t it = c->u.draw_indexed.index_type;
        const uint8_t *idx = (const uint8_t *)c->u.draw_indexed.indices;
        uint32_t n = c->u.draw_indexed.count;
        if (!idx || !n || n > 65536) continue;
        uint32_t maxi = 0;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t v;
            if (it == 0) v = idx[k];
            else if (it == 1) { uint16_t x = 0; memcpy(&x, idx + (size_t)k * 2, 2); v = x; }
            else { v = 0; memcpy(&v, idx + (size_t)k * 4, 4); }
            if (v > maxi) maxi = v;
        }
        if (maxi + 1 > need) need = maxi + 1;
        if (maxi < have) continue;
        size_t want = (size_t)(maxi + 1) * stride;
        if (nvn_guest_range_ok(addr, want)) continue;   /* extend from guest */
        uint32_t lim = have ? have - 1 : 0;
        uint8_t *w = (uint8_t *)(uintptr_t)idx;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t v;
            if (it == 0) v = w[k];
            else if (it == 1) { uint16_t x = 0; memcpy(&x, w + (size_t)k * 2, 2); v = x; }
            else { v = 0; memcpy(&v, w + (size_t)k * 4, 4); }
            if (v <= lim) continue;
            if (it == 0) w[k] = (uint8_t)lim;
            else if (it == 1) { uint16_t x = (uint16_t)lim; memcpy(w + (size_t)k * 2, &x, 2); }
            else memcpy(w + (size_t)k * 4, &lim, 4);
        }
        static unsigned cl;
        if (cl++ < 8)
            fprintf(stderr, "[totk-nvn] index clamp %s: max=%u have=%u stride=%u"
                            " (range %#llx+%zu not inside a pool)\n", where ? where : "?",
                    maxi, have, stride, (unsigned long long)addr, want);
    }
    return need;
}

uint32_t nvn_build_vertex_streams(const nvn_bind_state_t *b, GfxVertexStream *out,
                                         uint32_t max, void **owned, uint32_t max_owned,
                                         uint32_t *nowned, uint32_t draw_verts) {
    if (nowned) *nowned = 0;
    if (!b) return 0;
    uint32_t n = 0, no = 0, conv = 0;
    for (int i = 0; i < 16 && n < max; i++) {
        if (!(b->va_mask & (1u << i))) continue;
        uint32_t va = b->va[i];
        uint32_t fmt = va & 0xffu;
        uint32_t off = (va >> 8) & 0x7ffffu;
        uint32_t stream = (va >> 27) & 0x1fu;
        nvn_vfmt_t vf = nvn_vfmt_of((int)fmt);
        if (!vf.convert) continue;
        uint32_t bind = 24u + conv++;
        if (stream >= 16 || !(b->vb_mask & (1u << stream))) continue;
        uint64_t addr = b->vb[stream].address;
        size_t size = (size_t)b->vb[stream].size;
        uint32_t stride = (uint32_t)b->vs[stream];
        if (!addr || !stride) continue;
        uint32_t verts = (uint32_t)(size / stride);
        if (verts > (1u << 20)) verts = 1u << 20;
        size_t outsz = (size_t)verts * vf.comps * 4u;
        float *buf = outsz ? malloc(outsz) : NULL;
        if (!buf) continue;
        for (uint32_t v = 0; v < verts; v++) {
            const uint8_t *src = (const uint8_t *)(uintptr_t)addr + (size_t)v * stride + off;
            for (uint32_t c = 0; c < vf.comps; c++) {
                float fv = 0.0f;
                if (vf.bytes == 1) {
                    if (vf.is_signed) fv = (float)(int8_t)src[c];
                    else fv = (float)src[c];
                } else if (vf.bytes == 2) {
                    if (vf.is_signed) { int16_t x; memcpy(&x, src + c * 2, 2); fv = (float)x; }
                    else { uint16_t x; memcpy(&x, src + c * 2, 2); fv = (float)x; }
                } else {
                    if (vf.is_signed) { int32_t x; memcpy(&x, src + c * 4, 4); fv = (float)x; }
                    else { uint32_t x; memcpy(&x, src + c * 4, 4); fv = (float)x; }
                }
                buf[(size_t)v * vf.comps + c] = fv;
            }
        }
        out[n].binding = bind;
        out[n].data = buf;
        out[n].size = outsz;
        n++;
        if (owned && no < max_owned) owned[no++] = buf;
        else free(buf);
    }
    for (int i = 0; i < 16 && n < max; i++) {
        if (!(b->vb_mask & (1u << i))) continue;
        if (!b->vb[i].address || !b->vb[i].size) continue;
        size_t size = b->vb[i].size;
        if (size > (16u << 20)) size = 16u << 20;
        const void *data = (const void *)(uintptr_t)b->vb[i].address;
        /* The guest may bind a buffer whose declared size is smaller than the
         * draw range (TOTK's UI binds 32 bytes and draws 6+ vertices).  Pad a
         * host copy to the range so the driver never reads past the mapping. */
        uint32_t stride = b->vs[i] ? (uint32_t)b->vs[i] : 1;
        size_t need = (size_t)draw_verts * stride;
        if (need > size && need <= (64u << 20)) {
            uint8_t *copy = calloc(1, need);
            if (copy && owned && no < max_owned) {
                /* The bound range is usually a window into a larger ring; read
                 * the rest only when it is provably mapped. */
                size_t extra = need - size;
                if (nvn_guest_range_ok(b->vb[i].address + size, extra))
                    memcpy(copy, (const uint8_t *)data + size, extra);
                memcpy(copy, data, size);
                owned[no++] = copy;
                data = copy;
                size = need;
            } else {
                free(copy);
            }
        }
        out[n].binding = (uint32_t)i;
        out[n].data = data;
        out[n].size = size;
        n++;
    }
    if (nowned) *nowned = no;
    return n;
}

/* Find one stream's record-time snapshot in a draw command. */
const uint8_t *nvn_draw_vb_find(const nvn_cmd_t *c, int stream, size_t *size) {
    if (!c || !size) return NULL;
    if (c->kind == NVN_CMD_DRAW) {
        for (uint32_t k = 0; k < c->u.draw.vb_count && k < 4; k++)
            if (c->u.draw.vb_stream[k] == stream && c->u.draw.vb_data[k]) {
                *size = c->u.draw.vb_size[k];
                return c->u.draw.vb_data[k];
            }
    } else if (c->kind == NVN_CMD_DRAW_INDEXED) {
        for (uint32_t k = 0; k < c->u.draw_indexed.vb_count && k < 4; k++)
            if (c->u.draw_indexed.vb_stream[k] == stream && c->u.draw_indexed.vb_data[k]) {
                *size = c->u.draw_indexed.vb_size[k];
                return c->u.draw_indexed.vb_data[k];
            }
    }
    return NULL;
}
