/* NVN module: program_1. */
#include "nvn_internal.h"

uint32_t nvn_program_set_shaders(prog_t *p, int n, const void *shaders,
                                        size_t stride, const char *what) {
    if (!nvn_obj(p, NVN_MAGIC('P', 'R', 'O', 'G'), what)) return 0;
    if (n < 0 || n > 6 || (!shaders && n)) { nvn_fail(what, "invalid stage count"); return 0; }
    if (nvn_trace())
        fprintf(stderr, "[totk-nvn] %s entry p=%p n=%d stride=%zu thread=%#x\n",
                what, (void *)p, n, stride, runtime_thread_current_handle());
    if (nvn_trace_save_ui()) {
        uint64_t d0 = 0; const void *c0 = NULL;
        if (n > 0 && shaders) { memcpy(&d0, shaders, 8); memcpy(&c0, (const uint8_t *)shaders + 8, 8); }
        fprintf(stderr, "[totk-save-ui] set-shaders p=%p n=%d stride=%zu before_nstage=%u data0=%#llx ctl0=%p\n",
                (void *)p, n, stride, p->nstage, (unsigned long long)d0, c0);
    }
    p->nstage = (uint32_t)n;

    /* This path reads the offline shader cache (and, after the unlock below,
     * may create a pipeline) while holding g_prog_lock.  Do not keep a virtual
     * core across it: a waiter that owns a core and blocks on g_prog_lock is
     * the 5 s stall seen at boot.  The core is retaken when this call returns
     * to guest code (cleanup). */
    int away_sc __attribute__((cleanup(runtime_core_back))) = runtime_core_away();
    pthread_mutex_lock(&g_prog_lock);
    prog_shader_state_t *s = prog_state_find(p);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (s) s->owner = p;
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
        p->sd[i].data = data;
        p->sd[i].control = control;

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
        if (!have_code && g_nvn_probe && control && data >= 0x100000 &&
            runtime_shader_identity(control, (const void *)(uintptr_t)data, &id)) {
            /* Selftest control blocks are heap allocations, not mapped NVN
             * modules, so the low-address resolver rejects them.  The probe
             * stored its cache entry with the same direct identity. */
            code = (const void *)(uintptr_t)data;
            have_code = 1;
            resolved.why = "probe-synthetic";
        }
        int have_id = have_code && runtime_shader_identity(control, code, &id);
        if (nvn_trace_save_ui())
            fprintf(stderr, "[totk-save-ui] stage-resolve p=%p slot=%d have_code=%d have_id=%d stage=%u code=%p\n",
                    (void *)p, i, have_code, have_id, have_id ? id.stage : 99u, code);
        if (!have_id) {
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
            if (nvn_trace_frame() && hit <= 64)
                fprintf(stderr, "[totk-frame] program=%p stage=%s key=%s res=%s\n",
                        (void *)p, runtime_shader_stage_name(id.stage), id.key,
                        mod.res_path[0] ? mod.res_path : "-");
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
    int compute_ready = (s->loaded & (1u << 5)) != 0;
    if (ready && nvn_trace())
        fprintf(stderr, "[totk-nvn] program=%p has cached vs=%s ps=%s state=%p loaded=%#x\n",
                (void *)p, s->key[0], s->key[1], (void *)s, s->loaded);
    if (compute_ready && nvn_trace())
        fprintf(stderr, "[totk-nvn] program=%p has cached compute key=%s state=%p loaded=%#x\n",
                (void *)p, s->key[5], (void *)s, s->loaded);
    pthread_mutex_unlock(&g_prog_lock);
    /* A compute-only program (stage 5) never needs a graphics pipeline and must
     * not warm one: its pipeline is created on the first dispatch.  `ready`
     * still requires vs+ps, so a compute program never reaches this block. */
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
                nvn_program_cached_pipeline(p, 0x25, &warm_layout, 1, NULL, 0, "", 1);
            if (nvn_trace())
                fprintf(stderr, "[totk-nvn] warm pipeline program=%p -> %p\n",
                        (void *)p, (void *)rp);
        }
    }
    if (nvn_trace_save_ui()) {
        uint64_t f28 = 0, f30 = 0, f38 = 0;
        memcpy(&f28, (const uint8_t *)p + 0x28, 8);
        memcpy(&f30, (const uint8_t *)p + 0x30, 8);
        memcpy(&f38, (const uint8_t *)p + 0x38, 8);
        fprintf(stderr, "[totk-save-ui] prog-fields %p n=%d nstage=%u data0=%#llx ctl0=%p f28=%#llx f30=%#llx f38=%#llx\n",
                (void *)p, n, p->nstage, (unsigned long long)p->sd[0].data,
                p->sd[0].control, (unsigned long long)f28,
                (unsigned long long)f30, (unsigned long long)f38);
    }
    return 1;
}

uint32_t nvnProgramSetShaders(prog_t *p, int n, const void *shaders) {
    return nvn_program_set_shaders(p, n, shaders, 16, "nvnProgramSetShaders");
}

uint32_t nvnProgramSetShadersExt(prog_t *p, int n, const void *shaders) {
    /* TOTK's NVNshaderDataExt is 0x18 bytes: {data, control, reserved}. */
    return nvn_program_set_shaders(p, n, shaders, 0x18, "nvnProgramSetShadersExt");
}

int nvn_read_file(const char *path, uint8_t **out, size_t *size) {
    /* No runtime_core_away here: nvn_program_cached_pipeline calls this with
     * g_prog_lock held, and releasing the virtual core under a host mutex lets
     * the next thread block on that mutex while holding the core (lock/core
     * inversion).  The combined shader-cache path needs the lock split first. */
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
    prog_host_set(p, pipeline, NULL, NULL, NULL, 0);
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
    prog_host_set(p, NULL, NULL, NULL, pipeline, 0);
    fprintf(stderr, "[totk-nvn] loaded reflected SPIR-V vs=%s(%u) ps=%s(%u)\n",
            vs_path, nvs, ps_path, nps);
    return 1;
}

int runtime_nvn_program_bind_texture(void *prog, uint32_t set, uint32_t binding,
                                     const void *texture) {
    prog_t *p = prog;
    const tex_t *t = texture;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G') ||
        !t || t->magic != NVN_MAGIC('T', 'E', 'X', 0)) return 0;
    prog_host_t h = prog_host_view(p);
    if (!h.gfx_reflected_pipeline) return 0;
    GfxTexture *g = nvn_texture_gfx(t);
    if (!g) return 0;
    return gfx_backend_reflected_bind_texture(h.gfx_reflected_pipeline, set, binding, g);
}

int runtime_nvn_program_write_uniform(void *prog, uint32_t set, uint32_t binding,
                                      uint32_t offset, const void *data, size_t size) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G')) return 0;
    prog_host_t h = prog_host_view(p);
    if (!h.gfx_reflected_pipeline) return 0;
    return gfx_backend_reflected_write_uniform(h.gfx_reflected_pipeline,
                                               set, binding, offset, data, size);
}

int runtime_nvn_program_write_storage(void *prog, uint32_t set, uint32_t binding,
                                      uint32_t offset, const void *data, size_t size) {
    prog_t *p = prog;
    if (!p || p->magic != NVN_MAGIC('P', 'R', 'O', 'G')) return 0;
    prog_host_t h = prog_host_view(p);
    if (!h.gfx_reflected_pipeline) return 0;
    return gfx_backend_reflected_write_storage(h.gfx_reflected_pipeline,
                                               set, binding, offset, data, size);
}

void nvnProgramSetSampleShading(prog_t *p, int v) { (void)p; (void)v; }

int nvnProgramGetSubtileSize(const prog_t *p) { (void)p; return 0x1000; }

void nvnProgramSetSubroutineLinkage(prog_t *p, int n, const void *l) { (void)p; (void)n; (void)l; }

GfxReflectedPipeline *nvn_program_cached_pipeline(prog_t *p, uint32_t nvn_format,
                                                         const GfxRenderTargetLayout *layout,
                                                         int legacy_window_target,
                                                         const GfxVertexLayout *input,
                                                         uint64_t vertex_sig,
                                                         const char *variant,
                                                         int allow_create) {
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
    GfxReflectedPipeline *cached = prog_pipe_find(s, fmt, layout_sig, vertex_sig,
                                                  variant_sig);
    if (cached) {
        s->pipeline = cached;
        pthread_mutex_unlock(&g_prog_lock);
        return cached;
    }
    if (!allow_create) {
        if (nvn_trace_frame())
            fprintf(stderr, "[totk-frame] pipeline cache miss (create suppressed) program=%p"
                            " fmt=%#x layout=%#llx vertex=%#llx variant=%#llx\n",
                    (void *)p, fmt, (unsigned long long)layout_sig,
                    (unsigned long long)vertex_sig, (unsigned long long)variant_sig);
        pthread_mutex_unlock(&g_prog_lock);
        return NULL;
    }
    /* The creation path parses the shader/resource JSON and calls the Vulkan
     * driver.  A guest thread that wants g_prog_lock must not be left holding
     * a virtual core while this thread sits in that I/O: that is the host
     * lock / virtual core inversion seen as a 5 s stall (owner in
     * nvn_program_set_shaders -> __svfscanf, waiters queued for its core).
     * Release the core for the miss path only, then re-check the cache because
     * another thread may have published the same pipeline while we were out. */
    pthread_mutex_unlock(&g_prog_lock);
    int away __attribute__((cleanup(runtime_core_back))) = runtime_core_away();
    pthread_mutex_lock(&g_prog_lock);
    s = prog_state_find(p);
    if (!s || (s->loaded & ((1u << 0) | (1u << 1))) != ((1u << 0) | (1u << 1))) {
        pthread_mutex_unlock(&g_prog_lock);
        return NULL;
    }
    cached = prog_pipe_find(s, fmt, layout_sig, vertex_sig, variant_sig);
    if (cached) {
        s->pipeline = cached;
        pthread_mutex_unlock(&g_prog_lock);
        return cached;
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
    s->nspecs[0] = nvs > 64 ? 64 : nvs;
    s->nspecs[1] = nps > 64 ? 64 : nps;
    if (s->nspecs[0]) memcpy(s->specs[0], vss, s->nspecs[0] * sizeof vss[0]);
    if (s->nspecs[1]) memcpy(s->specs[1], pss, s->nspecs[1] * sizeof pss[0]);
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
    /* A non-window pass always uses a layout-created target (including a
     * single colour attachment), so the pipeline must be created for that
     * layout; only the legacy window swapchain image uses create_ex. */
    int use_layout = layout && !legacy_window_target;
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
        nvn_pipe_ubo_set(s->pipeline, vss, nvs, pss, nps);
        prog_pipe_add(s, s->pipeline, fmt, layout_sig, vertex_sig, variant_sig);
    }
    GfxReflectedPipeline *ready = s->pipeline;
    pthread_mutex_unlock(&g_prog_lock);
    return ready;
}
