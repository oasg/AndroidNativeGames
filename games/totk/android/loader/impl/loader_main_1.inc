/* loader_main: implementation phase 1. */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    crash_altstack_install();
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    fprintf(stderr, "[totk-android] main start\n");
    need(argc == 3, "usage: totk-android <rewritten-dir> probe");
    runtime_thread_init();
    fprintf(stderr, "[totk-android] thread init: host_ctx_off=%#llx guest_slots_off=%#llx\n",
            (unsigned long long)runtime_host_ctx_off, (unsigned long long)runtime_guest_slots_off);
    for (unsigned i = 0; i < NMODULES; i++) map_module(&modules[i], argv[1]);
    fprintf(stderr, "[totk-android] mapped all\n");
    for (unsigned i = 0; i < NMODULES; i++) relocate(&modules[i]);
    fprintf(stderr, "[totk-android] relocated all\n");
    for (unsigned i = 0; i < NMODULES; i++) seal(&modules[i]);
    fprintf(stderr, "[totk-android] sealed all\n");
    if (!strcmp(argv[2], "probe")) return probe();
    if (!strcmp(argv[2], "probe-hle")) return probe_hle();
    if (!strcmp(argv[2], "probe-vulkan-triangle")) return probe_vulkan_triangle(argv[1]);
    if (!strcmp(argv[2], "probe-totk-reflect")) {
        const char *vsp = getenv("TOTK_ANDROID_SPV_VS");
        const char *psp = getenv("TOTK_ANDROID_SPV_PS");
        const char *rvp = getenv("TOTK_ANDROID_REFL_VS");
        const char *rpp = getenv("TOTK_ANDROID_REFL_PS");
        if (!vsp || !psp || !rvp || !rpp) {
            fprintf(stderr, "[totk-vk] ERROR probe-totk-reflect needs SPV_VS/SPV_PS/REFL_VS/REFL_PS\n");
            return 1;
        }
        if (!reflect_check_interface(vsp, psp) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH"))
            return 1;
        size_t vsn = 0, psn = 0;
        uint8_t *vs = (uint8_t *)read_text_file(vsp, &vsn);
        uint8_t *ps = (uint8_t *)read_text_file(psp, &psn);
        if (!vs || !ps) { free(vs); free(ps); return 1; }
        gfx_spirv_move_set(ps, psn, 0, 1);
        GfxBindingSpec vss[64], pss[64];
        unsigned nvs = gfx_reflect_full(rvp, vsp, 0, vss, 64);
        unsigned nps = gfx_reflect_full(rpp, psp, 1, pss, 64);
        fprintf(stderr, "[totk-vk] reflected bindings vs=%u ps=%u\n", nvs, nps);
        reflect_log("vs", vss, nvs);
        reflect_log("ps", pss, nps);
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxReflectedPipeline *p = t ? gfx_backend_reflected_pipeline_create(
            vs, vsn, ps, psn, gfx_backend_vk_format_for_nvn(0x25), vss, nvs, pss, nps) : NULL;
        free(vs); free(ps);
        if (!t || !p) {
            if (t) gfx_backend_offscreen_destroy(t);
            fprintf(stderr, "[totk-vk] ERROR reflected pipeline\n");
            return 1;
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_reflected_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR reflected draw %d\n", i);
                gfx_backend_reflected_pipeline_destroy(p);
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        int visible = center[0] != corner[0] || center[1] != corner[1] || center[2] != corner[2];
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/reflect.ppm", argv[1]);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"totk-reflect\","
               "\"vs_bindings\":%u,\"ps_bindings\":%u,\"center\":\"%02x%02x%02x%02x\","
               "\"corner\":\"%02x%02x%02x%02x\",\"visible\":%s,\"screenshot\":\"%s\","
               "\"screenshot_ok\":%s}\n", nvs, nps, center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], visible ? "true" : "false",
               shot, shot_ok ? "true" : "false");
        gfx_backend_reflected_pipeline_destroy(p);
        gfx_backend_offscreen_destroy(t);
        return 0;
    }
    if (!strcmp(argv[2], "probe-totk-vs")) {
        const char *path = getenv("TOTK_ANDROID_SPV_VS");
        if (!path || !*path) path = "/data/local/tmp/totk-android/spv/system-vs-8511c97f.spv";
        int fd = open(path, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "[totk-vk] ERROR open %s\n", path); return 1; }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) { close(fd); return 1; }
        uint8_t *vs = malloc((size_t)st.st_size);
        ssize_t got = vs ? read(fd, vs, (size_t)st.st_size) : -1;
        close(fd);
        if (!vs || got != st.st_size) { free(vs); return 1; }
        const char *ps_path = getenv("TOTK_ANDROID_SPV_PS");
        if (ps_path && *ps_path &&
            !reflect_check_interface(path, ps_path) && !getenv("TOTK_ANDROID_ALLOW_INTERFACE_MISMATCH")) {
            free(vs);
            return 1;
        }
        uint8_t *ps = NULL;
        size_t ps_size = 0;
        if (ps_path && *ps_path) {
            int pfd = open(ps_path, O_RDONLY);
            if (pfd < 0 || fstat(pfd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)(16 << 20)) {
                if (pfd >= 0) close(pfd);
                free(vs);
                fprintf(stderr, "[totk-vk] ERROR open %s\n", ps_path);
                return 1;
            }
            ps_size = (size_t)st.st_size;
            ps = malloc(ps_size);
            ssize_t pgot = ps ? read(pfd, ps, ps_size) : -1;
            close(pfd);
            if (!ps || pgot != st.st_size) { free(vs); free(ps); return 1; }
        }
        GfxUboBinding ubos[8];
        uint32_t nubo = 0;
        const char *rv = getenv("TOTK_ANDROID_REFL_VS");
        const char *rp = getenv("TOTK_ANDROID_REFL_PS");
        if (((rv && reflect_has_non_ubo(rv)) || (rp && reflect_has_non_ubo(rp))) &&
            !getenv("TOTK_ANDROID_ALLOW_PARTIAL_REFLECTION")) {
            fprintf(stderr, "[totk-vk] ERROR probe-totk-vs only builds cbuf UBOs; "
                            "use probe-totk-reflect for sbufs/textures/images\n");
            free(vs); free(ps);
            return 1;
        }
        if (rv || rp) {
            uint32_t b[8];
            unsigned n = reflect_cbufs(rv, b, 8);
            for (unsigned i = 0; i < n; i++) ubos[nubo++] = (GfxUboBinding){ b[i], 0x10000 };
            n = reflect_cbufs(rp, b, 8);
            for (unsigned i = 0; i < n; i++) {
                int dup = 0;
                for (uint32_t j = 0; j < nubo; j++) if (ubos[j].binding == b[i]) dup = 1;
                if (!dup && nubo < 8) ubos[nubo++] = (GfxUboBinding){ b[i], 0x10000 };
            }
            fprintf(stderr, "[totk-vk] reflection cbufs: %u binding(s)\n", nubo);
        }
        if (!nubo) { ubos[nubo++] = (GfxUboBinding){1, 0x10000}; ubos[nubo++] = (GfxUboBinding){2, 0x10000}; }
        GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
        GfxTrianglePipeline *p = NULL;
        if (t) {
            if (ps) {
                p = gfx_backend_triangle_pipeline_create_ubo(vs, (size_t)got, ps, ps_size,
                                                             gfx_backend_vk_format_for_nvn(0x25), ubos, nubo);
            } else {
                p = gfx_backend_totk_vs_pipeline_create(vs, (size_t)got, gfx_backend_vk_format_for_nvn(0x25));
            }
        }
        free(vs); free(ps);
        if (!t || !p) { if (t) gfx_backend_offscreen_destroy(t); fprintf(stderr, "[totk-vk] ERROR TOTK VS pipeline\n"); return 1; }
        float c4[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        float c11[4] = {255.0f, 0.0f, 0.0f, 0.0f};
        if (nubo) {
            if (!gfx_backend_triangle_write_ubo(p, 1, 147 * 16, c4, sizeof c4) ||
                !gfx_backend_triangle_write_ubo(p, 2, 25 * 16, c11, sizeof c11)) {
                fprintf(stderr, "[totk-vk] ERROR TOTK VS UBO write\n");
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        }
        for (int i = 0; i < 10; i++)
            if (!gfx_backend_triangle_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
                fprintf(stderr, "[totk-vk] ERROR TOTK VS draw %d\n", i);
                gfx_backend_offscreen_destroy(t);
                return 1;
            }
        uint8_t center[4] = {0}, corner[4] = {0};
        gfx_backend_offscreen_read_pixel(t, 640, 360, center);
        gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
        char shot[PATH_MAX];
        const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
        if (env && *env) snprintf(shot, sizeof shot, "%s", env);
        else snprintf(shot, sizeof shot, "%s/totk-vs.ppm", argv[1]);
        int ok = ps_path && *ps_path
                 ? (center[0] > 200 && center[1] > 200 && center[2] > 200)
                 : (center[0] > 200 && center[1] > 60 && center[1] < 100 && center[2] < 40);
        int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
        printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"totk-vs\","
               "\"ps\":\"%s\",\"center\":\"%02x%02x%02x%02x\",\"corner\":\"%02x%02x%02x%02x\","
               "\"screenshot\":\"%s\",\"screenshot_ok\":%s}\n",
               ok ? "pass" : "fail", (ps_path && *ps_path) ? ps_path : "builtin",
               center[0], center[1], center[2], center[3],
               corner[0], corner[1], corner[2], corner[3], shot, shot_ok ? "true" : "false");
        gfx_backend_offscreen_destroy(t);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[2], "probe-vulkan-reflect-texture")) {
        if (!gfx_backend_reflected_texture_selftest()) {
            fprintf(stderr, "[totk-vk] reflected texture selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-reflect-texture\"}\n");
        return 0;
    }
    if (!strcmp(argv[2], "probe-vulkan-reflect-ubo")) {
        if (!gfx_backend_reflected_ubo_selftest()) {
            fprintf(stderr, "[totk-vk] reflected UBO selftest FAIL\n");
            return 1;
        }
        printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"vulkan-reflect-ubo\"}\n");
        return 0;
    }
