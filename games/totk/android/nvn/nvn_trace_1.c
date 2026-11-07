/* NVN module: trace_1. */
#include "nvn_internal.h"

void nvn_trace_segment_inputs(const char *tag, cb_t *view,
                                     const nvn_bind_state_t *bs, prog_t *p,
                                     GfxReflectedPipeline *rp) {
    char vs[96] = "-", ps[96] = "-", variant[192] = "-";
    if (p) {
        pthread_mutex_lock(&g_prog_lock);
        prog_shader_state_t *ss = prog_state_find(p);
        if (ss) {
            if (ss->key[0][0]) snprintf(vs, sizeof vs, "%s", ss->key[0]);
            if (ss->key[1][0]) snprintf(ps, sizeof ps, "%s", ss->key[1]);
            if (ss->variant[0]) snprintf(variant, sizeof variant, "%s", ss->variant);
        }
        pthread_mutex_unlock(&g_prog_lock);
    }
    fprintf(stderr, "[totk-frame] %s program=%p pipeline=%p vs=%s ps=%s variant=%s"
                    " writes tex=%u map=%u b2b=%u b2t=%u drawtex=%u cleartex=%u\n",
            tag, (void *)p, (void *)rp, vs, ps, variant,
            nvn_tex_writes, nvn_buf_maps, nvn_cb_copy_b2b, nvn_cb_copy_b2t,
            nvn_cb_draw_texture_calls, nvn_cb_clear_texture_calls);
    const tex_pool_t *tpool = nvn_tex_pool_for(view);
    for (int st = 0; st < 2; st++) {
        for (int i = 0; i < 128; i++) {
            if (!(bs->tex_mask[st][i / 32] & (1u << (i % 32)))) continue;
            uint64_t h = bs->tex[st][i];
            int tid = (int)(h & 0xfffff);
            const tex_t *t = tpool ? nvn_tex_pool_lookup(tpool, tid) : NULL;
            if (!t) {
                fprintf(stderr, "[totk-frame] %s   tex st=%d slot=%d handle=%#llx"
                                " tid=%d unresolved tpool=%p\n",
                        tag, st, i, (unsigned long long)h, tid, (const void *)tpool);
                continue;
            }
            size_t msize = t->L.size ? (size_t)t->L.size
                                     : (size_t)t->d.width * t->d.height * 4;
            uint32_t nz = nvn_first_nonzero(t->mem, msize, 1u << 20);
            GfxTexture *g = (GfxTexture *)nvn_texture_gfx(t);
            const tex_bridge_t *b = nvn_tex_bridge(t, 0);
            char src[64];
            if (!g) snprintf(src, sizeof src, "none");
            else if (b && b->rt_valid)
                snprintf(src, sizeof src, "rt-%s=%llu",
                         b->rt_frame == b->rt_uploaded ? "gen" : "pending",
                         (unsigned long long)b->rt_frame);
            else snprintf(src, sizeof src, "guest");
            fprintf(stderr, "[totk-frame] %s   tex st=%d slot=%d handle=%#llx tid=%d"
                            " tex=%p tgt=%d fmt=%#x %ux%u lv=%u layers=%u"
                            " mem=%p off=%lld msize=%zu first_nz=%s%u gfx=%p src=%s\n",
                    tag, st, i, (unsigned long long)h, tid, (const void *)t,
                    t->d.target, t->d.format, t->d.width, t->d.height, t->d.levels,
                    t->L.layers, (void *)t->mem, (long long)t->off, msize,
                    nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz,
                    (void *)g, src);
        }
    }
    for (int st = 0; st < 2; st++) {
        for (int i = 0; i < 16; i++) {
            if (!(bs->ubo_mask[st] & (1u << i))) continue;
            uint64_t a = bs->ubo[st][i].address;
            size_t n = bs->ubo[st][i].size;
            uint32_t nz = nvn_first_nonzero((const uint8_t *)(uintptr_t)a, n, 1u << 20);
            uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            if (a && n >= 4) memcpy(&w0, (const void *)(uintptr_t)a, 4);
            if (a && n >= 8) memcpy(&w1, (const void *)(uintptr_t)a + 4, 4);
            if (a && n >= 12) memcpy(&w2, (const void *)(uintptr_t)a + 8, 4);
            if (a && n >= 16) memcpy(&w3, (const void *)(uintptr_t)a + 12, 4);
            fprintf(stderr, "[totk-frame] %s   ubo st=%d slot=%d addr=%#llx size=%zu"
                            " first=%08x %08x %08x %08x first_nz=%s%u\n",
                    tag, st, i, (unsigned long long)a, n, w0, w1, w2, w3,
                    nz == UINT32_MAX ? "none " : "", nz == UINT32_MAX ? 0 : nz);
            if (a && st == 0 && n >= 224) {
                const uint32_t *w = (const uint32_t *)(uintptr_t)a;
                fprintf(stderr, "[totk-frame] %s   vs-ubo idx9=%08x %08x %08x %08x"
                                " idx10=%08x %08x %08x %08x idx13=%08x %08x %08x %08x"
                                " idx34=%08x %08x %08x %08x\n", tag,
                        w[9*4+0], w[9*4+1], w[9*4+2], w[9*4+3],
                        w[10*4+0], w[10*4+1], w[10*4+2], w[10*4+3],
                        w[13*4+0], w[13*4+1], w[13*4+2], w[13*4+3],
                        w[34*4+0], w[34*4+1], w[34*4+2], w[34*4+3]);
            }
            if (a && st == 1 && n >= 16) {
                uint32_t words[36] = { 0 };
                size_t nw = n / 4;
                if (nw > 36) nw = 36;
                memcpy(words, (const void *)(uintptr_t)a, nw * 4);
                fprintf(stderr, "[totk-frame] %s   ps-ubo:", tag);
                for (size_t k = 0; k < nw; k++) fprintf(stderr, " %08x", words[k]);
                fputc('\n', stderr);
            }
        }
    }
    int vp[4] = {0}, sc[4] = {0};
    for (uint32_t ci = 0; ci < view->ncmds; ci++) {
        const nvn_cmd_t *cc = &view->cmds[ci];
        if (cc->kind == NVN_CMD_SET_VIEWPORT) {
            vp[0] = cc->u.rect.x; vp[1] = cc->u.rect.y;
            vp[2] = cc->u.rect.width; vp[3] = cc->u.rect.height;
        } else if (cc->kind == NVN_CMD_SET_SCISSOR) {
            sc[0] = cc->u.rect.x; sc[1] = cc->u.rect.y;
            sc[2] = cc->u.rect.width; sc[3] = cc->u.rect.height;
        }
    }
    GfxVertexLayout input;
    uint64_t sig = 0;
    nvn_build_vertex_layout(bs, &input, &sig);
    fprintf(stderr, "[totk-frame] %s   state vp=%d,%d %dx%d sc=%d,%d %dx%d"
                    " blend_en=%u blend=%llu src=%llu dst=%llu eq=%llu cull=%u"
                    " front=%u chmask=%#x va=%#x vb=%#x attrs=%u streams=%u\n",
            tag, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3],
            (unsigned)bs->rs.color.blend_en, (unsigned long long)bs->rs.blend.target,
            (unsigned long long)bs->rs.blend.src_rgb, (unsigned long long)bs->rs.blend.dst_rgb,
            (unsigned long long)bs->rs.blend.eq_rgb, (unsigned)bs->rs.poly.cull,
            (unsigned)bs->rs.poly.front, bs->rs.chmask.mask, bs->va_mask,
            bs->vb_mask, input.nattributes, input.nbindings);
    for (uint32_t ai = 0; ai < input.nattributes; ai++)
        fprintf(stderr, "[totk-frame] %s   attr %u loc=%u bind=%u fmt=%#x off=%u va=%#x nvnfmt=%#x\n",
                tag, ai, input.attributes[ai].location, input.attributes[ai].binding,
                input.attributes[ai].format, input.attributes[ai].offset,
                bs->va[input.attributes[ai].location],
                bs->va[input.attributes[ai].location] & 0xff);
    for (uint32_t bi = 0; bi < input.nbindings; bi++)
        fprintf(stderr, "[totk-frame] %s   vbind %u b=%u stride=%u rate=%u\n", tag, bi,
                input.bindings[bi].binding, input.bindings[bi].stride,
                input.bindings[bi].input_rate);
    for (int vi = 0; vi < 16; vi++) {
        if (!(bs->vb_mask & (1u << vi))) continue;
        uint64_t va = bs->vb[vi].address;
        uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
        if (va) {
            memcpy(&w0, (const void *)(uintptr_t)va, 4);
            memcpy(&w1, (const void *)(uintptr_t)va + 4, 4);
            memcpy(&w2, (const void *)(uintptr_t)va + 8, 4);
            memcpy(&w3, (const void *)(uintptr_t)va + 12, 4);
        }
        fprintf(stderr, "[totk-frame] %s   vb %d addr=%#llx size=%zu first=%08x %08x %08x %08x\n",
                tag, vi, (unsigned long long)va, bs->vb[vi].size, w0, w1, w2, w3);
    }
}

void nvn_trace_draw_instanced(void *c, int p, int t, int count, void *idx,
                                     int bv, int bi, int inst) {
    (void)c; (void)p; (void)t; (void)bv; (void)bi;
    nvn_zero_call("nvnCommandBufferDrawElementsInstanced");
    if (nvn_trace_frame())
        fprintf(stderr, "[totk-frame] draw indexed instanced count=%d index=%p instances=%d\n",
                count, idx, inst);
}

void nvn_trace_draw_indirect(void *c, void *a) {
    (void)c; (void)a; nvn_zero_call("nvnCommandBufferDrawElementsIndirect");
}

void nvn_trace_draw_arrays_indirect(void *c, void *a) {
    (void)c; (void)a; nvn_zero_call("nvnCommandBufferDrawArraysIndirect");
}

void nvn_trace_draw_texture(void *c, uint64_t h, const void *src, const void *dst) {
    nvn_cb_draw_texture_calls++;
    uint32_t s[16] = {0}, d[16] = {0};
    if (src) memcpy(s, src, sizeof s);
    if (dst) memcpy(d, dst, sizeof d);
    const tex_pool_t *tp = nvn_tex_pool_for((const cb_t *)c);
    const tex_t *t = tp ? nvn_tex_pool_lookup(tp, (int)(h & 0xfffff)) : NULL;
    static unsigned logged;
    if (nvn_trace_composite() && logged++ < 64)
        fprintf(stderr, "[totk-drawtex] cb=%p h=%#llx tex=%p fmt=%#x %ux%u mem=%p"
                        " src=%p[%u %u %u %u | f %.3f %.3f %.3f %.3f]"
                        " dst=%p[%u %u %u %u | f %.3f %.3f %.3f %.3f]\n",
                c, (unsigned long long)h, (const void *)t, t ? (unsigned)t->d.format : 0,
                t ? t->d.width : 0, t ? t->d.height : 0, t ? (void *)t->mem : NULL,
                src, s[0], s[1], s[2], s[3],
                *(const float *)&s[0], *(const float *)&s[1],
                *(const float *)&s[2], *(const float *)&s[3],
                dst, d[0], d[1], d[2], d[3],
                *(const float *)&d[0], *(const float *)&d[1],
                *(const float *)&d[2], *(const float *)&d[3]);
    nvn_zero_call("nvnCommandBufferDrawTexture");
}
