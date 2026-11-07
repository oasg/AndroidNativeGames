/* NVN module: probe_4. */
#include "nvn_internal.h"

/* Focused regression check for the two first-frame state bugs:
 *  - the NVNvertexAttribState/StreamState setters actually fill the packed
 *    objects the command buffer binds (attrs used to arrive as zero), and
 *  - texture/sampler pools are inherited when the recording command buffer
 *    has not set a pool of its own. */
int runtime_nvn_vertex_state_selftest(void) {
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;

    uint32_t va = 0xdeadbeefu;
    uint64_t vs = 0xdeadbeefcafef00dull;
    nvnVertexAttribStateSetDefaults(&va);
    nvnVertexAttribStateSetFormat(&va, 0x29, 0);
    nvnVertexAttribStateSetStreamIndex(&va, 0);
    nvnVertexStreamStateSetDefaults(&vs);
    nvnVertexStreamStateSetStride(&vs, 16);
    nvnVertexStreamStateSetDivisor(&vs, 0);
    int fmt = -1;
    ptrdiff_t off = -1;
    nvnVertexAttribStateGetFormat(&va, &fmt, &off);
    int ok = fmt == 0x29 && off == 0 && nvnVertexAttribStateGetStreamIndex(&va) == 0 &&
             nvnVertexStreamStateGetStride(&vs) == 16 && nvnVertexStreamStateGetDivisor(&vs) == 0;

    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    nvnCommandBufferBindVertexAttribState(&cb, 1, &va);
    nvnCommandBufferBindVertexStreamState(&cb, 1, &vs);
    nvnCommandBufferBindVertexBuffer(&cb, 0, 0x1000, 64);
    const nvn_bind_state_t *bs = nvn_cb_draw_state(&cb);
    GfxVertexLayout l;
    uint64_t sig = 0;
    nvn_build_vertex_layout(bs, &l, &sig);
    int want_vk = gfx_backend_vk_format_for_nvn(0x29);
    int layout_ok = l.nattributes == 1 && l.nbindings == 1 && sig != 0 &&
                    l.attributes[0].location == 0 && l.attributes[0].binding == 0 &&
                    l.attributes[0].format == want_vk && l.attributes[0].offset == 0 &&
                    l.bindings[0].binding == 0 && l.bindings[0].stride == 16;
    ok &= layout_ok;

    /* Pool inheritance across command buffers. */
    tex_pool_t tp;
    samp_pool_t sp;
    tex_t tex;
    samp_t samp;
    memset(&tp, 0, sizeof tp);
    memset(&sp, 0, sizeof sp);
    memset(&tex, 0, sizeof tex);
    memset(&samp, 0, sizeof samp);
    tp.magic = NVN_MAGIC('T', 'P', 'O', 'L'); tp.n = 4;
    sp.magic = NVN_MAGIC('S', 'P', 'O', 'L'); sp.n = 4;
    tex.magic = NVN_MAGIC('T', 'E', 'X', 0);
    samp.magic = NVN_MAGIC('S', 'A', 'M', 'P');
    nvn_tex_pool_add(&tp, 3, &tex);
    nvn_samp_pool_add(&sp, 2, &samp);
    cb_t pool_cb;
    cb_t draw_cb;
    memset(&pool_cb, 0, sizeof pool_cb);
    memset(&draw_cb, 0, sizeof draw_cb);
    if (!nvnCommandBufferInitialize(&pool_cb, &d) ||
        !nvnCommandBufferInitialize(&draw_cb, &d))
        return 0;
    nvnCommandBufferSetTexturePool(&pool_cb, &tp);
    nvnCommandBufferSetSamplerPool(&pool_cb, &sp);
    uint64_t handle = ((uint64_t)2 << 20) | 3u;   /* sampler 2, texture 3 */
    nvnCommandBufferBindTexture(&draw_cb, 1, 0, handle);
    const nvn_bind_state_t *dbs = nvn_cb_draw_state(&draw_cb);
    const tex_pool_t *tpool = nvn_tex_pool_for(&draw_cb);
    const samp_pool_t *spool = nvn_samp_pool_for(&draw_cb);
    int pool_ok = tpool == &tp && spool == &sp &&
                  (dbs->tex_mask[1][0] & 1u) && dbs->tex[1][0] == handle &&
                  nvn_tex_pool_lookup(tpool, 3) == &tex &&
                  nvn_samp_pool_lookup(spool, 2) == &samp;
    ok &= pool_ok;

    fprintf(stderr, "[totk-nvn] vertex-state probe attrs=%u bindings=%u fmt=%#x stride=%u"
                    " sig=%#llx layout=%d pool=%d -> %s\n",
            l.nattributes, l.nbindings, l.attributes[0].format,
            l.nbindings ? l.bindings[0].stride : 0, (unsigned long long)sig,
            layout_ok, pool_ok, ok ? "PASS" : "FAIL");
    nvn_tex_pool_forget(&tp);
    nvn_samp_pool_forget(&sp);
    nvn_pool_forget(&tp);
    nvn_pool_forget(&sp);
    nvnCommandBufferFinalize(&pool_cb);
    nvnCommandBufferFinalize(&draw_cb);
    nvnCommandBufferFinalize(&cb);
    nvnDeviceFinalize(&d);
    return ok;
}

int runtime_nvn_retired_stress_selftest(int iterations) {
    if (iterations < 1) iterations = 1;
    if (iterations > 2000) iterations = 2000;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    if (!gfx_backend_vulkan_available()) return 0;
    const uint32_t W = 64, H = 64;
    tex_t A, B, C, A2;
    if (!nvn_seg_probe_tex(&A, W, H) || !nvn_seg_probe_tex(&B, W, H) ||
        !nvn_seg_probe_tex(&C, W, H)) return 0;
    A2 = A;
    nvn_tex_registry_add(&A2);
    GfxRenderTargetLayout layout;
    memset(&layout, 0, sizeof layout);
    layout.color_count = 1;
    layout.color_formats[0] = (uint32_t)gfx_backend_vk_format_for_nvn(0x25);
    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    GfxReflectedPipeline *pipe = (vs && ps)
        ? gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size,
                                                       &layout, NULL, 0, NULL, 0, NULL)
        : NULL;
    if (!pipe) return 0;
    prog_t prog;
    memset(&prog, 0, sizeof prog);
    if (!nvnProgramInitialize(&prog, &d)) return 0;
    prog_host_set(&prog, NULL, NULL, NULL, pipe, 0);
    const void *tA[1] = { &A };
    const void *tB[1] = { &B };
    const void *tC[1] = { &C };
    const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    const float blue[4] = { 0, 0, 1, 1 };
    unsigned rendered = 0, submitted = 0, segs = 0;
    int ok = 1;
    for (int it = 0; it < iterations && ok; it++) {
        cb_t cb;
        memset(&cb, 0, sizeof cb);
        if (!nvnCommandBufferInitialize(&cb, &d)) { ok = 0; break; }
        cb.prog = &prog;
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferSetRenderTargets(&cb, 1, tA, NULL, NULL, NULL);
        nvnCommandBufferClearColor(&cb, 0, red, 0xf);
        nvnCommandBufferSetViewport(&cb, 0, 0, (int)W, (int)H);
        nvnCommandBufferSetScissor(&cb, 0, 0, (int)W, (int)H);
        nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        nvnCommandBufferSetRenderTargets(&cb, 1, tB, NULL, NULL, NULL);
        nvnCommandBufferClearColor(&cb, 0, green, 0xf);
        nvnCommandBufferSetViewport(&cb, 0, 0, (int)W, (int)H);
        nvnCommandBufferSetScissor(&cb, 0, 0, (int)W, (int)H);
        nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
        nvnCommandBufferSetRenderTargets(&cb, 1, tC, NULL, NULL, NULL);
        nvnCommandBufferClearColor(&cb, 0, blue, 0xf);
        uint64_t handle = nvnCommandBufferEndRecording(&cb);
        nvnCommandBufferFinalize(&cb);
        unsigned r0 = nvn_scene_rendered, s0 = nvn_scene_submits;
        for (int k = 0; k < 4; k++) nvn_scene_submit_pending();
        rendered += nvn_scene_rendered - r0;
        submitted += nvn_scene_submits - s0;
        segs += 3;
        if (nvn_scene_rendered - r0 != 2 || nvn_scene_submits - s0 != 3) ok = 0;
        (void)handle;
    }
    GfxOffscreenTarget *rtA = nvn_host_rt_get_owner(W, H, &layout, &A);
    GfxOffscreenTarget *rtB = nvn_host_rt_get_owner(W, H, &layout, &B);
    GfxTexture *ga = nvn_texture_gfx_ready(&A2);
    const tex_bridge_t *ba = nvn_tex_bridge(&A, 0);
    const tex_bridge_t *ba2 = nvn_tex_bridge(&A2, 0);
    uint8_t ac[4] = {0}, bc[4] = {0};
    if (rtA) gfx_backend_offscreen_read_pixel_index(rtA, 0, 4, 4, ac);
    if (rtB) gfx_backend_offscreen_read_pixel_index(rtB, 0, 4, 4, bc);
    int alias = ba && ba2 && ba == ba2 && ba2->rt_valid;
    if (!rtA || !rtB || !ga || !alias ||
        ac[0] < 180 || ac[1] > 80 || ac[2] > 80 ||
        bc[1] < 180 || bc[0] > 80 || bc[2] > 80) ok = 0;
    fprintf(stderr, "[totk-nvn] retired stress iters=%d segs=%u rendered=%u"
                    " submitted=%u alias=%d A=%02x%02x%02x B=%02x%02x%02x ok=%d\n",
            iterations, segs, rendered, submitted, alias, ac[0], ac[1], ac[2],
            bc[0], bc[1], bc[2], ok);
    if (ga) nvn_texture_bridge_drop(&A2);
    nvnTextureFinalize(&A);
    nvnTextureFinalize(&A2);
    nvnTextureFinalize(&B);
    nvnTextureFinalize(&C);
    free(A.mem);
    free(B.mem);
    free(C.mem);
    nvnProgramFinalize(&prog);
    fprintf(stderr, "[totk-nvn] retired stress selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* probe-nvn-prog-abi: validate the guest-visible NVNprogram layout and that
 * host pipeline/texture state stays in the side map, not in the guest object. */
int runtime_nvn_prog_abi_selftest(void) {
    if (sizeof(prog_t) > 0xC0) return 0;
    if (offsetof(prog_t, sd[0]) != 0x08 ||
        offsetof(prog_t, sd[2].data) != 0x28 ||
        offsetof(prog_t, sd[2].control) != 0x30 ||
        offsetof(prog_t, serial) != 0x68) return 0;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d)) return 0;
    uint8_t ctl[0x880];
    nvn_shader_data_t sd[2] = {
        { 0x1111, ctl },
        { 0x2222, ctl },
    };
    if (!nvnProgramSetShaders(&p, 2, sd)) return 0;
    int ok = p.magic == NVN_MAGIC('P', 'R', 'O', 'G') && p.nstage == 2 &&
             p.sd[0].data == 0x1111 && p.sd[0].control == ctl &&
             p.sd[1].data == 0x2222 && p.sd[1].control == ctl;
    for (int i = 2; i < 6; i++)
        if (p.sd[i].data || p.sd[i].control) ok = 0;
    prog_shader_state_t *s = NULL;
    pthread_mutex_lock(&g_prog_lock);
    s = prog_state_find(&p);
    pthread_mutex_unlock(&g_prog_lock);
    if (!s || s->owner != &p) ok = 0;
    /* Host-only resources must survive independently of the guest bytes. */
    prog_host_set(&p, (void *)0x1, (void *)0x2, (void *)0x3, NULL, 1);
    prog_host_t h = prog_host_view(&p);
    if (h.gfx_pipeline != (void *)0x1 || h.gfx_textured_pipeline != (void *)0x2 ||
        h.gfx_texture != (void *)0x3 || h.textured != 1) ok = 0;
    if (p.sd[2].data || p.sd[2].control) ok = 0;
    nvnProgramFinalize(&p);
    pthread_mutex_lock(&g_prog_lock);
    s = prog_state_find(&p);
    pthread_mutex_unlock(&g_prog_lock);
    if (s) ok = 0;
    fprintf(stderr, "[totk-nvn] prog ABI size=%zu sd2=(%#zx,%#zx) serial=%#zx ok=%d\n",
            sizeof(prog_t), offsetof(prog_t, sd[2].data),
            offsetof(prog_t, sd[2].control), offsetof(prog_t, serial), ok);
    fprintf(stderr, "[totk-nvn] prog ABI selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int runtime_nvn_sparse_target_selftest(void) {
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    tex_t A, B, D;
    if (!nvn_seg_probe_tex(&A, 64, 64) || !nvn_seg_probe_tex(&B, 64, 64) ||
        !nvn_seg_probe_tex(&D, 64, 64)) return 0;
    void *tg[4] = { &A, &B, NULL, &D };
    GfxRenderTargetLayout layout;
    uint32_t w = 0, h = 0;
    const char *why = NULL;
    int ok = nvn_rt_layout_from_targets(4, tg, NULL, NULL, NULL, &layout, &w, &h, &why);
    ok = ok && layout.color_count == 4 && w == 64 && h == 64 &&
         layout.color_formats[0] && layout.color_formats[1] &&
         layout.color_formats[2] == layout.color_formats[0] &&
         layout.color_formats[3];
    void *tail[4] = { &A, &B, NULL, NULL };
    GfxRenderTargetLayout tl;
    uint32_t tw = 0, th = 0;
    const char *twhy = NULL;
    ok = ok && nvn_rt_layout_from_targets(4, tail, NULL, NULL, NULL, &tl, &tw, &th, &twhy) &&
         tl.color_count == 2;
    GfxOffscreenTarget *rt = ok ? nvn_host_rt_get_owner(64, 64, &layout, &A) : NULL;
    if (!rt) ok = 0;
    fprintf(stderr, "[totk-nvn] sparse MRT colors=%u dummy=%#x/%#x tail=%u rt=%p ok=%d%s%s\n",
            layout.color_count, layout.color_formats[0], layout.color_formats[2],
            tl.color_count, (void *)rt, ok, why ? " why=" : "", why ? why : "");
    nvnTextureFinalize(&A);
    nvnTextureFinalize(&B);
    nvnTextureFinalize(&D);
    free(A.mem); free(B.mem); free(D.mem);
    fprintf(stderr, "[totk-nvn] sparse target selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int runtime_nvn_segment_selftest(const char *screenshot, int frames) {
    (void)frames;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) return 0;
    if (!gfx_backend_vulkan_available()) return 0;
    const uint32_t W = 64, H = 64;
    tex_t A, B, C, D;
    if (!nvn_seg_probe_tex(&A, W, H) || !nvn_seg_probe_tex(&B, W, H) ||
        !nvn_seg_probe_tex(&C, W, H) || !nvn_seg_probe_tex(&D, W, H)) {
        return 0;
    }
    /* Distinct object aliasing A's storage: the sampled path must find A's RT. */
    tex_t A2 = A;
    nvn_tex_registry_add(&A2);
    GfxRenderTargetLayout layout;
    memset(&layout, 0, sizeof layout);
    layout.color_count = 1;
    layout.color_formats[0] = (uint32_t)gfx_backend_vk_format_for_nvn(0x25);
    if (!layout.color_formats[0]) return 0;
    const uint8_t *vs = NULL, *ps = NULL;
    size_t vs_size = 0, ps_size = 0;
    gfx_backend_builtin_triangle_modules(&vs, &vs_size, &ps, &ps_size);
    GfxReflectedPipeline *pipe = (vs && ps)
        ? gfx_backend_reflected_pipeline_create_layout(vs, vs_size, ps, ps_size,
                                                       &layout, NULL, 0, NULL, 0, NULL)
        : NULL;
    if (!pipe) { fprintf(stderr, "[totk-nvn] segment probe: no pipeline\n"); return 0; }
    prog_t prog;
    memset(&prog, 0, sizeof prog);
    if (!nvnProgramInitialize(&prog, &d)) return 0;
    prog_host_set(&prog, NULL, NULL, NULL, pipe, 0);
    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) return 0;
    cb.prog = &prog;

    const void *tA[1] = { &A };
    const void *tB[1] = { &B };
    const void *tC[1] = { &C };
    const void *tD[1] = { &D };
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const float blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferSetRenderTargets(&cb, 1, tA, NULL, NULL, NULL);
    nvnCommandBufferClearColor(&cb, 0, red, 0xf);
    nvnCommandBufferSetViewport(&cb, 0, 0, (int)W, (int)H);
    nvnCommandBufferSetScissor(&cb, 0, 0, (int)W, (int)H);
    nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
    nvnCommandBufferSetRenderTargets(&cb, 1, tB, NULL, NULL, NULL);
    nvnCommandBufferClearColor(&cb, 0, green, 0xf);
    nvnCommandBufferSetViewport(&cb, 0, 0, (int)W, (int)H);
    nvnCommandBufferSetScissor(&cb, 0, 0, (int)W, (int)H);
    nvnCommandBufferDrawArrays(&cb, NVN_DRAW_TRIANGLES, 0, 3);
    /* Clear-only targets: execute the clears in order and associate the RTs so
     * a later sample sees the cleared image. */
    nvnCommandBufferSetRenderTargets(&cb, 1, tC, NULL, NULL, NULL);
    nvnCommandBufferClearColor(&cb, 0, blue, 0xf);
    nvnCommandBufferSetRenderTargets(&cb, 1, tD, NULL, NULL, NULL);
    nvnCommandBufferClearColor(&cb, 0, white, 0xf);
    uint64_t handle = nvnCommandBufferEndRecording(&cb);
    nvnCommandBufferFinalize(&cb);

    int ok = 1;
    uint32_t nseg = 0, queued = 0;
    pthread_mutex_lock(&g_retired_lock);
    nvn_retired_t *e = NULL;
    for (unsigned i = 0; i < NVN_RETIRED_MAX; i++)
        if (g_retired[i].used && g_retired[i].handle == handle) { e = &g_retired[i]; break; }
    if (!e) ok = 0;
    if (ok) {
        nseg = e->nseg;
        if (nseg != 4) ok = 0;
        if (nseg >= 4) {
            if (e->seg[0].color0_tex != &A || e->seg[0].draw_count != 3) ok = 0;
            if (e->seg[1].color0_tex != &B || e->seg[1].draw_count != 3) ok = 0;
            if (e->seg[2].color0_tex != &C || e->seg[2].draw_count != 0 ||
                e->seg[2].clear_count == 0) ok = 0;
            if (e->seg[3].color0_tex != &D || e->seg[3].draw_count != 0 ||
                e->seg[3].clear_count == 0) ok = 0;
        }
        for (uint32_t i = 0; i < nseg; i++)
            if ((e->seg[i].draw_count > 0 || e->seg[i].clear_count > 0) &&
                e->seg[i].have_layout) queued++;
    }
    pthread_mutex_unlock(&g_retired_lock);

    unsigned rendered_before = nvn_scene_rendered;
    unsigned submits_before = nvn_scene_submits;
    /* The production drain is serialized (one segment per submit); the probe
     * must submit repeatedly to drain all four segments. */
    for (int k = 0; k < 4; k++) nvn_scene_submit_pending();
    unsigned rendered = nvn_scene_rendered - rendered_before;
    unsigned submitted = nvn_scene_submits - submits_before;

    GfxOffscreenTarget *rtA = nvn_host_rt_get_owner(W, H, &layout, &A);
    GfxOffscreenTarget *rtB = nvn_host_rt_get_owner(W, H, &layout, &B);
    GfxOffscreenTarget *rtC = nvn_host_rt_get_owner(W, H, &layout, &C);
    GfxOffscreenTarget *rtD = nvn_host_rt_get_owner(W, H, &layout, &D);
    if (!rtA || !rtB || rtA == rtB || !rtC || !rtD) {
        fprintf(stderr, "[totk-nvn] segment probe: host targets A=%p B=%p C=%p D=%p\n",
                (void *)rtA, (void *)rtB, (void *)rtC, (void *)rtD);
        ok = 0;
    }
    uint8_t a_corner[4] = {0}, b_corner[4] = {0};
    uint8_t a_center[4] = {0}, b_center[4] = {0};
    uint8_t c_corner[4] = {0}, d_corner[4] = {0};
    if (rtA && rtB) {
        ok &= gfx_backend_offscreen_read_pixel_index(rtA, 0, 4, 4, a_corner) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(rtB, 0, 4, 4, b_corner) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(rtA, 0, W / 2, H / 2, a_center) != 0;
        ok &= gfx_backend_offscreen_read_pixel_index(rtB, 0, W / 2, H / 2, b_center) != 0;
        if (rtC) ok &= gfx_backend_offscreen_read_pixel_index(rtC, 0, 4, 4, c_corner) != 0;
        if (rtD) ok &= gfx_backend_offscreen_read_pixel_index(rtD, 0, 4, 4, d_corner) != 0;
    }
    int a_red = a_corner[0] > 180 && a_corner[1] < 80 && a_corner[2] < 80;
    int b_green = b_corner[1] > 180 && b_corner[0] < 80 && b_corner[2] < 80;
    int centers_drawn = (a_center[0] || a_center[1] || a_center[2]) &&
                        (b_center[0] || b_center[1] || b_center[2]);
    int c_blue = c_corner[2] > 180 && c_corner[0] < 80 && c_corner[1] < 80;
    int d_white = d_corner[0] > 180 && d_corner[1] > 180 && d_corner[2] > 180;
    if (!a_red || !b_green || !centers_drawn || !c_blue || !d_white) ok = 0;
    if (submitted != 4) ok = 0;

    /* Sample paths must use the segment that drew to each texture. */
    GfxTexture *ga = nvn_texture_gfx_ready(&A);
    GfxTexture *gb = nvn_texture_gfx_ready(&B);
    GfxTexture *gd = nvn_texture_gfx_ready(&D);
    GfxTexture *ga2 = nvn_texture_gfx_ready(&A2);
    const tex_bridge_t *ba = nvn_tex_bridge(&A, 0);
    const tex_bridge_t *bb = nvn_tex_bridge(&B, 0);
    const tex_bridge_t *bd = nvn_tex_bridge(&D, 0);
    const tex_bridge_t *ba2 = nvn_tex_bridge(&A2, 0);
    int a_gen = ba && ba->rt_valid && ba->rt_uploaded == ba->rt_frame;
    int b_gen = bb && bb->rt_valid && bb->rt_uploaded == bb->rt_frame;
    int d_gen = bd && bd->rt_valid && bd->rt_uploaded == bd->rt_frame;
    int alias = ba2 && ba2 == ba && ba2->rt_valid;
    if (!ga || !gb || !gd || !ga2 || !a_gen || !b_gen || !d_gen || !alias) ok = 0;

    if (screenshot && *screenshot && rtA && rtB) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s.c0.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(rtA, 0, path)) ok = 0;
        snprintf(path, sizeof path, "%s.c1.ppm", screenshot);
        if (!gfx_backend_offscreen_write_ppm_index(rtB, 0, path)) ok = 0;
    }
    fprintf(stderr, "[totk-nvn] segment probe segs=%u queued=%u rendered=%u"
                    " A=%02x%02x%02x B=%02x%02x%02x centerA=%02x%02x%02x"
                    " centerB=%02x%02x%02x C=%02x%02x%02x D=%02x%02x%02x"
                    " submitted=%u txtA=%p txtB=%p alias=%d ok=%d\n",
            nseg, queued, rendered, a_corner[0], a_corner[1], a_corner[2],
            b_corner[0], b_corner[1], b_corner[2],
            a_center[0], a_center[1], a_center[2], b_center[0], b_center[1],
            b_center[2], c_corner[0], c_corner[1], c_corner[2],
            d_corner[0], d_corner[1], d_corner[2], submitted,
            (void *)ga, (void *)gb, alias, ok);
    if (ga) nvn_texture_bridge_drop(&A);
    if (gb) nvn_texture_bridge_drop(&B);
    if (gd) nvn_texture_bridge_drop(&D);
    nvn_tex_registry_del(&A);
    nvn_tex_registry_del(&A2);
    nvn_tex_registry_del(&B);
    nvn_tex_registry_del(&C);
    nvn_tex_registry_del(&D);
    free(A.mem);
    free(B.mem);
    free(C.mem);
    free(D.mem);
    nvnProgramFinalize(&prog);
    fprintf(stderr, "[totk-nvn] segment selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
