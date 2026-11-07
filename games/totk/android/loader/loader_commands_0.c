/* Module: commands_0. */
#include "loader_internal.h"

void *probe_thread(void *unused) {
    (void)unused;
    runtime_thread_init();
    typedef uint64_t (*Fn)(void);
    Fn fn = (Fn)(void *)(modules[0].base + PROBE_PLT);
    uint64_t previous = 0;
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t current = fn();
        need(current >= previous && current > 0, "cross-module clock result");
        previous = current;
    }
    return NULL;
}

void loader_ready(void) {
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"sdk-start\",\"stage\":\"loader-ready\"}\n");
    fflush(stdout);
    if (!inits_enabled()) {
        runtime_hle_trace_report();
        _exit(0);
    }
}

int probe_vulkan_triangle(const char *dir) {
    const char *fs = getenv("TOTK_ANDROID_TRIANGLE_FRAMES");
    int frames = fs && *fs ? atoi(fs) : 60;
    if (frames < 1) frames = 1;
    if (frames > 600) frames = 600;
    GfxOffscreenTarget *t = gfx_backend_offscreen_create(1280, 720, 0x25);
    if (!t) { fprintf(stderr, "[totk-vk] ERROR triangle target\n"); return 1; }
    GfxTrianglePipeline *p = gfx_backend_builtin_triangle_pipeline();
    if (!p) { gfx_backend_offscreen_destroy(t); return 1; }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < frames; i++) {
        if (!gfx_backend_triangle_draw(p, t, 0.05f, 0.08f, 0.12f, 1.0f)) {
            fprintf(stderr, "[totk-vk] ERROR triangle draw frame %d\n", i);
            gfx_backend_offscreen_destroy(t);
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double sec = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    uint8_t center[4] = {0}, corner[4] = {0};
    gfx_backend_offscreen_read_pixel(t, 640, 360, center);
    gfx_backend_offscreen_read_pixel(t, 0, 0, corner);
    uint64_t hash = gfx_backend_offscreen_hash(t);
    char shot[PATH_MAX];
    const char *env = getenv("TOTK_ANDROID_SCREENSHOT");
    if (env && *env) snprintf(shot, sizeof shot, "%s", env);
    else snprintf(shot, sizeof shot, "%s/triangle.ppm", dir);
    int shot_ok = gfx_backend_offscreen_write_ppm(t, shot);
    int ok = center[0] > 200 && center[1] > 60 && center[1] < 100 &&
             center[2] < 40 && corner[0] < 40 && corner[1] < 40;
    printf("{\"status\":\"%s\",\"host\":\"android\",\"probe\":\"vulkan-triangle\","
           "\"frames\":%d,\"fps\":%.2f,\"center\":\"%02x%02x%02x%02x\","
           "\"corner\":\"%02x%02x%02x%02x\",\"hash\":\"%016llx\",\"screenshot\":\"%s\","
           "\"screenshot_ok\":%s}\n",
           ok ? "pass" : "fail", frames, sec > 0 ? frames / sec : 0.0,
           center[0], center[1], center[2], center[3],
           corner[0], corner[1], corner[2], corner[3],
           (unsigned long long)hash, shot, shot_ok ? "true" : "false");
    gfx_backend_offscreen_destroy(t);
    return ok ? 0 : 1;
}

int boot_sdk(void) {
    uint32_t main_handle = svc_thread_main_init();
    runtime_thread_set_guest_handle(main_handle);
    void *start = runtime_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    need(start != NULL, "nn::init::Start symbol");
    fprintf(stderr, "[totk-android] ENTER nn::init::Start\n");
    runtime_guest_call(start, runtime_guest_stack_top(), main_handle, 0,
                       (uint64_t)(uintptr_t)loader_ready,
                       (uint64_t)(uintptr_t)module_inits,
                       (uint64_t)(uintptr_t)module_finis);
    runtime_fail("nn::init::Start returned");
    return 1;
}

int probe_hle(void) {
    typedef uint64_t (*Fn)(uint64_t, uint64_t);
    Fn f = (Fn)runtime_hle_wrap_named((void *)host_add, "host_add");
    need(f != NULL, "HLE thunk");
    uint64_t r = f(3, 4);
    need(r == 7, "HLE thunk result");
    printf("{\"status\":\"pass\",\"host\":\"android\",\"probe\":\"hle-stack\",\"result\":7}\n");
    return 0;
}

int probe(void) {
    uintptr_t slot;
    memcpy(&slot, modules[0].base + PROBE_SLOT, 8);
    void *required = runtime_symbol("_ZN2nn2os13GetSystemTickEv");
    need(required != NULL, "GetSystemTick symbol");
    need(slot == (uintptr_t)required, "main PLT GOT resolution");
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) need(pthread_create(&threads[i], NULL, probe_thread, NULL) == 0, "probe thread");
    for (unsigned i = 0; i < 4; i++) need(pthread_join(threads[i], NULL) == 0, "join probe");
    need(timer_calls == 4000, "timer gate count");
    printf("{\"status\":\"pass\",\"host\":\"android\",\"execution\":\"native arm64 main PLT -> sdk -> host timer -> return\","
           "\"calls\":4000,\"threads\":4,\"modules\":[");
    for (unsigned i = 0; i < NMODULES; i++) {
        Module *m = &modules[i];
        printf("%s{\"name\":\"%s\",\"relative\":%u,\"symbolic\":%u,\"cross\":%u,\"weak_zero\":%u}",
               i ? "," : "", m->name, m->relative, m->symbolic, m->cross, m->weak);
    }
    puts("],\"scope\":\"cross-module calls; constructors and game entry not run\"}");
    return 0;
}
