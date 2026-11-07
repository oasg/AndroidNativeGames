/* NVN module: compute_0. */
#include "nvn_internal.h"

#include "gfx_compute_spv.h"

/* Build (or reuse) the compute pipeline of one program state.  Reflection uses
 * the same .res.json/SPIR-V path as the graphics pipelines and all compute
 * bindings live in descriptor set 0. */
GfxComputePipeline *nvn_compute_pipeline_get(prog_shader_state_t *s,
                                                    GfxBindingSpec *out, uint32_t *nout) {
    if (!s) return NULL;
    GfxComputePipeline *cached = NULL;
    uint32_t ncached = 0;
    GfxBindingSpec specs[64];
    uint32_t nspecs = 0;
    pthread_mutex_lock(&g_prog_lock);
    cached = s->compute_pipeline;
    ncached = s->compute_nspecs;
    if (cached && ncached) memcpy(specs, s->compute_specs, ncached * sizeof *specs);
    uint8_t *spv = s->spv[5];
    size_t spv_size = s->spv_size[5];
    if (!cached && spv && spv_size) {
        const char *res = s->res_path[5][0] ? s->res_path[5] : NULL;
        const char *spv_path = s->spv_path[5][0] ? s->spv_path[5] : NULL;
        nspecs = gfx_reflect_full(res, spv_path, 0, specs, 64);
        /* The SPIR-V-only reflection in loader/ classifies a legacy
         * BufferBlock SSBO (OpDecorate without a value word) as a uniform
         * block.  The .res.json section is authoritative for "cbuf vs sbuf",
         * so take the buffer kind from there when the two disagree. */
        GfxBindingSpec json[64];
        unsigned njson = gfx_reflect_full(res, NULL, 0, json, 64);
        for (uint32_t i = 0; i < nspecs; i++) {
            for (unsigned j = 0; j < njson; j++) {
                if (json[j].binding != specs[i].binding) continue;
                if ((json[j].kind == GFX_BIND_UBO || json[j].kind == GFX_BIND_SSBO) &&
                    specs[i].kind != json[j].kind)
                    specs[i].kind = json[j].kind;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_prog_lock);
    if (cached) {
        if (out && ncached) memcpy(out, specs, ncached * sizeof *out);
        if (nout) *nout = ncached;
        if (nvn_gpu_trace())
            for (uint32_t i = 0; i < ncached; i++)
                fprintf(stderr, "[totk-nvn] compute spec[%u] binding=%u kind=%d count=%u"
                                " nvn_slot=%d cbuf=%d off=%d\n", i, specs[i].binding,
                        (int)specs[i].kind, specs[i].count, specs[i].nvn_slot,
                        specs[i].nvn_cbuf_slot, specs[i].nvn_cb_offset);
        return cached;
    }
    if (!spv || !spv_size) return NULL;
    GfxComputePipeline *cp = gfx_backend_compute_pipeline_create(spv, spv_size, specs, nspecs);
    if (!cp) {
        fprintf(stderr, "[totk-nvn] compute: pipeline creation failed (spv=%zu bindings=%u)\n",
                spv_size, nspecs);
        return NULL;
    }
    pthread_mutex_lock(&g_prog_lock);
    if (s->compute_pipeline) {
        /* Another thread won the race; keep the first one. */
        gfx_backend_compute_pipeline_destroy(cp);
        cp = s->compute_pipeline;
        ncached = s->compute_nspecs;
        if (out && ncached) memcpy(out, s->compute_specs, ncached * sizeof *out);
        if (nout) *nout = ncached;
    } else {
        s->compute_pipeline = cp;
        s->compute_nspecs = nspecs;
        if (nspecs) memcpy(s->compute_specs, specs, nspecs * sizeof *specs);
        if (out && nspecs) memcpy(out, specs, nspecs * sizeof *out);
        if (nout) *nout = nspecs;
    }
    pthread_mutex_unlock(&g_prog_lock);
    if (nvn_gpu_trace()) {
        fprintf(stderr, "[totk-nvn] compute pipeline %p bindings=%u\n", (void *)cp, nspecs);
        for (uint32_t i = 0; i < nspecs; i++)
            fprintf(stderr, "[totk-nvn] compute spec[%u] binding=%u kind=%d count=%u"
                            " nvn_slot=%d cbuf=%d off=%d\n", i, specs[i].binding,
                    (int)specs[i].kind, specs[i].count, specs[i].nvn_slot,
                    specs[i].nvn_cbuf_slot, specs[i].nvn_cb_offset);
    }
    return cp;
}

int nvn_copy_probe(const char *work_dir) {
    const uint32_t W = 32, H = 32;
    const size_t BYTES = (size_t)W * H * 4;
    fprintf(stderr, "[totk-nvn] copy probe begin work=%s\n", work_dir ? work_dir : "(null)");
    int ok = 1;

    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: device\n");
        return 1;
    }
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: queue\n");
        return 1;
    }
    /* Guest-visible buffer storage for the upload/download round trip. */
    uint8_t pool_mem[0x20000] __attribute__((aligned(4096)));
    pool_t pool;
    pool_builder_t pb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    nvnMemoryPoolBuilderSetDefaults(&pb);
    nvnMemoryPoolBuilderSetDevice(&pb, &d);
    nvnMemoryPoolBuilderSetStorage(&pb, pool_mem, sizeof pool_mem);
    if (!nvnMemoryPoolInitialize(&pool, &pb)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: memory pool\n");
        return 1;
    }
    buf_t buf_src, buf_dst;
    buf_builder_t bb;
    memset(&buf_src, 0, sizeof buf_src);
    memset(&buf_dst, 0, sizeof buf_dst);
    memset(&bb, 0, sizeof bb);
    nvnBufferBuilderSetDefaults(&bb);
    nvnBufferBuilderSetDevice(&bb, &d);
    nvnBufferBuilderSetStorage(&bb, &pool, 0, BYTES);
    if (!nvnBufferInitialize(&buf_src, &bb)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: source buffer\n");
        return 1;
    }
    memset(&bb, 0, sizeof bb);
    nvnBufferBuilderSetDefaults(&bb);
    nvnBufferBuilderSetDevice(&bb, &d);
    nvnBufferBuilderSetStorage(&bb, &pool, (ptrdiff_t)BYTES, BYTES);
    if (!nvnBufferInitialize(&buf_dst, &bb)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: destination buffer\n");
        return 1;
    }
    uint8_t *pat_a = malloc(BYTES), *pat_b = malloc(BYTES), *pat_q = malloc(16 * 16 * 4);
    uint8_t *got = malloc(BYTES);
    if (!pat_a || !pat_b || !pat_q || !got) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: allocation\n");
        return 1;
    }
    nvn_probe_fill(pat_a, W, H, 0x20);
    nvn_probe_fill(pat_b, W, H, 0x60);
    /* A buffer->texture copy consumes a tightly packed region: 16x16 texels. */
    nvn_probe_fill(pat_q, 16, 16, 0xa0);

    tex_t tex_a, tex_b;
    memset(&tex_a, 0, sizeof tex_a);
    memset(&tex_b, 0, sizeof tex_b);
    if (!nvn_probe_make_texture(&tex_a, W, H, 0x25, pat_a) ||
        !nvn_probe_make_texture(&tex_b, W, H, 0x25, pat_b)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: texture creation\n");
        ok = 0;
    }

    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) {
        fprintf(stderr, "[totk-nvn] copy probe FAIL: command buffer\n");
        ok = 0;
    }

    /* Source buffer holds the tightly packed Q pattern for CopyBufferToTexture. */
    memcpy(pool_mem, pat_q, 16 * 16 * 4);
    uint8_t view[16];
    memset(view, 0, sizeof view);

    if (ok) {
        const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        int32_t clear_reg[6] = { 0, 0, 0, 8, 8, 1 };
        int32_t src_reg[6] = { 8, 8, 0, 16, 16, 1 };
        int32_t dst_reg[6] = { 16, 16, 0, 16, 16, 1 };
        int32_t b2t_reg[6] = { 0, 16, 0, 16, 16, 1 };
        int32_t t2b_reg[6] = { 0, 0, 0, (int32_t)W, (int32_t)H, 1 };
        nvnCommandBufferBeginRecording(&cb);
        nvn_trace_clear_texture(&cb, &tex_b, view, (void *)clear_reg, (void *)red, 0xf);
        nvn_trace_copy_t2t(&cb, &tex_a, view, src_reg, &tex_b, view, dst_reg, 0);
        nvn_trace_copy_b2t(&cb, &buf_src, &tex_b, view, (void *)b2t_reg, 0);
        nvn_trace_copy_t2b(&cb, &tex_b, view, t2b_reg, &buf_dst, 0);
        uint64_t h = nvnCommandBufferEndRecording(&cb);
        nvnQueueSubmitCommands(&q, 1, &h);
    }

    if (ok) {
        const uint8_t *out = (const uint8_t *)nvnBufferMap(&buf_dst);
        if (!out) {
            fprintf(stderr, "[totk-nvn] copy probe FAIL: destination buffer not mapped\n");
            ok = 0;
        } else {
            for (uint32_t y = 0; y < H && ok; y++) {
                for (uint32_t x = 0; x < W && ok; x++) {
                    const uint8_t *want;
                    if (x < 8 && y < 8) {
                        static const uint8_t redpx[4] = { 255, 0, 0, 255 };
                        want = redpx;
                    } else if (x >= 16 && y >= 16) {
                        want = pat_a + ((size_t)(y - 16 + 8) * W + (x - 16 + 8)) * 4;
                    } else if (y >= 16 && x < 16) {
                        want = pat_q + ((size_t)(y - 16) * 16 + x) * 4;
                    } else {
                        want = pat_b + ((size_t)y * W + x) * 4;
                    }
                    const uint8_t *have = out + ((size_t)y * W + x) * 4;
                    if (memcmp(have, want, 4)) {
                        fprintf(stderr, "[totk-nvn] copy probe FAIL: [%u,%u] got="
                                        "%02x%02x%02x%02x want=%02x%02x%02x%02x\n",
                                x, y, have[0], have[1], have[2], have[3],
                                want[0], want[1], want[2], want[3]);
                        ok = 0;
                    }
                }
            }
            if (ok) {
                fprintf(stderr, "[totk-nvn] copy probe: %ux%u every byte matches"
                                " (clear + t2t + b2t + t2b)\n", W, H);
                memcpy(got, out, BYTES);
            }
        }
    }

    fprintf(stderr, "[totk-nvn] copy probe stats: clear=%u/%u copy=%u/%u upload=%u/%u"
                    " download=%u/%u unsupported=%u rt=%u\n",
            g_gpu_stats.clear_ok, g_gpu_stats.clear_fail,
            g_gpu_stats.copy_ok, g_gpu_stats.copy_fail,
            g_gpu_stats.upload_ok, g_gpu_stats.upload_fail,
            g_gpu_stats.download_ok, g_gpu_stats.download_fail,
            g_gpu_stats.unsupported, g_gpu_stats.rt_unsupported);
    if (ok && work_dir && *work_dir) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/copy-probe-out.bin", work_dir);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(got, 1, BYTES, f);
            fclose(f);
            fprintf(stderr, "[totk-nvn] copy probe wrote %s\n", path);
        }
    }

    nvnCommandBufferFinalize(&cb);
    nvnTextureFinalize(&tex_a);
    nvnTextureFinalize(&tex_b);
    nvnBufferFinalize(&buf_src);
    nvnBufferFinalize(&buf_dst);
    nvnMemoryPoolFinalize(&pool);
    nvnQueueFinalize(&q);
    nvnDeviceFinalize(&d);
    free(pat_a); free(pat_b); free(pat_q); free(got);
    fprintf(stderr, "[totk-nvn] copy probe %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Synthetic compute program: the repository's compute_probe.comp, bound to a
 * UBO, an SSBO and a sampled texture, dispatched through the NVN command
 * buffer.  The SSBO is read back from guest memory and checked byte-exactly. */
int nvn_compute_probe(const char *work_dir) {
    fprintf(stderr, "[totk-nvn] compute probe begin work=%s\n", work_dir ? work_dir : "(null)");
    int ok = 1;
    nvn_dev_t d;
    dev_builder_t db;
    memset(&d, 0, sizeof d);
    memset(&db, 0, sizeof db);
    if (!nvnDeviceInitialize(&d, &db)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: device\n");
        return 1;
    }
    queue_t q;
    queue_builder_t qb;
    memset(&q, 0, sizeof q);
    memset(&qb, 0, sizeof qb);
    qb.dev = &d; qb.flags = 1; qb.cmd_sz = 0x10000; qb.ctl_sz = 0x1000; qb.mem_sz = 0x2000;
    if (!nvnQueueInitialize(&q, &qb)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: queue\n");
        return 1;
    }

    /* Synthetic stage-5 NVN shader identity + cache entry holding the
     * repository's compute SPIR-V and its reflection JSON. */
    const char res_json[] =
        "{\"cbufs\":[{\"binding\":0,\"slot\":0}],"
        "\"sbufs\":[{\"binding\":1,\"slot\":0}],"
        "\"textures\":[{\"binding\":2,\"slot\":0,\"type\":0}],\"images\":[]}";
    uint8_t *control = calloc(1, 0x880);
    uint8_t *code = calloc(1, 0x100);
    if (!control || !code) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: allocation\n");
        return 1;
    }
    uint32_t declared = 0x880, c1 = 0, length = 0x100, stage = 5;
    memcpy(control + 0x20, &declared, sizeof declared);
    memcpy(control + 0x700, &c1, sizeof c1);
    memcpy(control + 0x704, &length, sizeof length);
    memcpy(control + 0x714, &stage, sizeof stage);
    memset(code, 0x5a, length);
    RuntimeShaderIdentity id;
    if (!runtime_shader_identity(control, code, &id) || id.stage != 5) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: synthetic identity\n");
        free(control); free(code);
        return 1;
    }
    if (!runtime_shader_cache_store(runtime_shader_cache_root(), &id,
                                    (const uint8_t *)compute_probe_comp_spv,
                                    compute_probe_comp_spv_size, res_json)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: cache store\n");
        free(control); free(code);
        return 1;
    }

    prog_t p;
    memset(&p, 0, sizeof p);
    if (!nvnProgramInitialize(&p, &d) ||
        !nvn_probe_install_stage(&p, 5, (const uint8_t *)compute_probe_comp_spv,
                                 compute_probe_comp_spv_size, res_json, &id)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: program install\n");
        free(control); free(code);
        return 1;
    }
    p.nstage = 1;

    /* Guest buffers: params (UBO), output (SSBO), plus a small texture. */
    static uint8_t pool_mem[0x20000] __attribute__((aligned(4096)));
    pool_t pool;
    pool_builder_t pb;
    memset(&pool, 0, sizeof pool);
    memset(&pb, 0, sizeof pb);
    nvnMemoryPoolBuilderSetDefaults(&pb);
    nvnMemoryPoolBuilderSetDevice(&pb, &d);
    nvnMemoryPoolBuilderSetStorage(&pb, pool_mem, sizeof pool_mem);
    if (!nvnMemoryPoolInitialize(&pool, &pb)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: memory pool\n");
        return 1;
    }
    typedef struct { uint32_t width, height, seed, pad; float scale[4]; } probe_params_t;
    probe_params_t *params = (probe_params_t *)pool_mem;
    uint32_t *out = (uint32_t *)(pool_mem + 0x1000);
    memset(out, 0, 8 * 8 * 4);
    params->width = 8;
    params->height = 8;
    params->seed = 0x12345678u;
    params->pad = 0;
    params->scale[0] = params->scale[1] = params->scale[2] = params->scale[3] = 1.0f;

    buf_t buf_ubo, buf_ssbo;
    buf_builder_t bb;
    memset(&bb, 0, sizeof bb);
    nvnBufferBuilderSetDefaults(&bb);
    nvnBufferBuilderSetDevice(&bb, &d);
    nvnBufferBuilderSetStorage(&bb, &pool, 0, sizeof *params);
    if (!nvnBufferInitialize(&buf_ubo, &bb)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: ubo buffer\n");
        return 1;
    }
    memset(&bb, 0, sizeof bb);
    nvnBufferBuilderSetDefaults(&bb);
    nvnBufferBuilderSetDevice(&bb, &d);
    nvnBufferBuilderSetStorage(&bb, &pool, 0x1000, 8 * 8 * 4);
    if (!nvnBufferInitialize(&buf_ssbo, &bb)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: ssbo buffer\n");
        return 1;
    }

    uint8_t texels[4 * 4 * 4];
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            uint8_t *px = texels + ((size_t)y * 4 + x) * 4;
            px[0] = (uint8_t)((x + y * 4u) * 16u);
            px[1] = (uint8_t)(x * 32u);
            px[2] = (uint8_t)(y * 64u);
            px[3] = 255;
        }
    tex_t tex;
    memset(&tex, 0, sizeof tex);
    if (!nvn_probe_make_texture(&tex, 4, 4, 0x25, texels)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: texture\n");
        return 1;
    }

    tex_pool_t tp;
    memset(&tp, 0, sizeof tp);
    if (!nvnTexturePoolInitialize(&tp, &pool, 0, 2)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: texture pool\n");
        return 1;
    }
    /* Pool index 1: handle 0 means "unbind" in NVN, so the probe binds 1. */
    nvnTexturePoolRegisterTexture(&tp, 1, &tex, NULL);

    cb_t cb;
    memset(&cb, 0, sizeof cb);
    if (!nvnCommandBufferInitialize(&cb, &d)) {
        fprintf(stderr, "[totk-nvn] compute probe FAIL: command buffer\n");
        return 1;
    }
    nvnCommandBufferSetTexturePool(&cb, &tp);

    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferBindProgram(&cb, &p, -1);
    nvnCommandBufferBindUniformBuffer(&cb, 5, 0,
                                      (uint64_t)(uintptr_t)params, sizeof *params);
    nvnCommandBufferBindStorageBuffer(&cb, 5, 0, (uint64_t)(uintptr_t)out, 8 * 8 * 4);
    nvnCommandBufferBindImage(&cb, 5, 0, 1);   /* pool index 1, no sampler */
    nvn_trace_dispatch_compute(&cb, 1, 1, 1);
    uint64_t h = nvnCommandBufferEndRecording(&cb);
    nvnQueueSubmitCommands(&q, 1, &h);

    for (uint32_t y = 0; y < 8 && ok; y++)
        for (uint32_t x = 0; x < 8 && ok; x++) {
            uint32_t texv = texels[(((y & 3u) * 4u) + (x & 3u)) * 4u];
            uint32_t want = (x + 1u) * 3u + (y + 1u) * 5u + params->seed + texv * 7u;
            uint32_t have = out[y * 8 + x];
            if (want != have) {
                fprintf(stderr, "[totk-nvn] compute probe FAIL: ssbo[%u,%u] got=%08x"
                                " want=%08x\n", x, y, have, want);
                ok = 0;
            }
        }
    if (ok)
        fprintf(stderr, "[totk-nvn] compute probe: SSBO 8x8 exact match\n");
    fprintf(stderr, "[totk-nvn] compute probe stats: dispatch=%u/%u unsupported=%u\n",
            g_gpu_stats.dispatch_ok, g_gpu_stats.dispatch_fail, g_gpu_stats.unsupported);

    nvnCommandBufferFinalize(&cb);
    nvn_tex_pool_forget(&tp);
    nvnTextureFinalize(&tex);
    nvnBufferFinalize(&buf_ubo);
    nvnBufferFinalize(&buf_ssbo);
    nvnMemoryPoolFinalize(&pool);
    nvnProgramFinalize(&p);
    nvnQueueFinalize(&q);
    nvnDeviceFinalize(&d);
    free(control);
    free(code);
    fprintf(stderr, "[totk-nvn] compute probe %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
