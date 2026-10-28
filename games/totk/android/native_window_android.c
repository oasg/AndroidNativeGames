/* Host NativeWindow abstraction for the Android CPU-only path.
 *
 * The real Horizon native window is an Android Parcel carrying a dispdrv binder
 * object.  This host has no binder/compositor, so Phase 2A uses an explicit
 * offscreen description instead: width, height, format and a host framebuffer.
 * When the Android app shell supplies a real ANativeWindow, `android` is set and
 * the same description is also used for the first on-screen presentation.
 */
#include "runtime.h"
#include "gfx_backend_android.h"
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_NW_MAGIC 0x30574e48u /* "HNW0" */

typedef struct HostNativeWindow {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* NVN format id (0x25 = RGBA8) */
    uint32_t bpp;
    uint32_t buffer_count;
    uint32_t stride;        /* bytes */
    uint32_t offscreen;
    uint64_t layer_id;
    void *android;          /* ANativeWindow*, set by the app shell */
    int surface_ok;         /* VkSwapchainKHR created for `android` */
    void *gfx_target;       /* GfxOffscreenTarget* or NULL for the software path */
    unsigned presents;      /* successful surface/offscreen presents */
    unsigned surface_presents; /* presents that went through the swapchain */
    float clear[4];
    uint8_t *buffer;
    size_t buffer_size;
} HostNativeWindow;

/* Host windows are the only pointers the NVN/VI handoff may pass back as a
 * native window.  A guest pointer or an old VI handle must not be dereferenced
 * to test it. */
static HostNativeWindow *g_host_windows[64];
static unsigned g_host_window_count;
static pthread_mutex_t g_host_window_lock = PTHREAD_MUTEX_INITIALIZER;

/* App-shell owned host window used when the caller only has a Surface.  The
 * game path normally passes the HostNativeWindow it got from VI. */
static HostNativeWindow *g_shell_window;

int runtime_native_window_valid(const void *p) {
    if (!p) return 0;
    int found = 0;
    pthread_mutex_lock(&g_host_window_lock);
    for (unsigned i = 0; i < g_host_window_count; i++)
        if (g_host_windows[i] == p) { found = 1; break; }
    pthread_mutex_unlock(&g_host_window_lock);
    return found;
}

static void host_window_register(HostNativeWindow *w) {
    if (!w) return;
    pthread_mutex_lock(&g_host_window_lock);
    if (g_host_window_count < sizeof g_host_windows / sizeof g_host_windows[0])
        g_host_windows[g_host_window_count++] = w;
    pthread_mutex_unlock(&g_host_window_lock);
}

static void host_window_unregister(HostNativeWindow *w) {
    pthread_mutex_lock(&g_host_window_lock);
    for (unsigned i = 0; i < g_host_window_count; i++) {
        if (g_host_windows[i] != w) continue;
        g_host_windows[i] = g_host_windows[--g_host_window_count];
        break;
    }
    pthread_mutex_unlock(&g_host_window_lock);
}

void *runtime_native_window_create(uint32_t width, uint32_t height, uint32_t format, uint64_t layer_id) {
    if (!width || !height || width > 16384 || height > 16384) return NULL;
    HostNativeWindow *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->magic = HOST_NW_MAGIC;
    w->width = width;
    w->height = height;
    w->format = format ? format : 0x25;
    w->bpp = 4;
    w->buffer_count = 2;
    w->stride = width * w->bpp;
    w->offscreen = 1;
    w->layer_id = layer_id;
    w->buffer_size = (size_t)w->stride * height;
    w->buffer = calloc(1, w->buffer_size ? w->buffer_size : 1);
    if (!w->buffer) { free(w); return NULL; }
    w->clear[0] = 0.0f; w->clear[1] = 0.0f; w->clear[2] = 0.0f; w->clear[3] = 1.0f;

    const char *want = getenv("TOTK_ANDROID_GFX_BACKEND");
    int force_vulkan = want && !strcmp(want, "vulkan");
    int force_software = want && !strcmp(want, "software");
    if (!force_software) {
        if (gfx_backend_vulkan_init()) {
            w->gfx_target = gfx_backend_offscreen_create(width, height, w->format);
            if (!w->gfx_target) {
                if (force_vulkan) {
                    free(w->buffer); free(w);
                    fprintf(stderr, "[totk-vi] ERROR Vulkan offscreen target creation failed\n");
                    return NULL;
                }
                fprintf(stderr, "[totk-vi] Vulkan target unsupported; using software framebuffer\n");
            }
        } else if (force_vulkan) {
            free(w->buffer); free(w);
            fprintf(stderr, "[totk-vi] ERROR Vulkan requested but unavailable\n");
            return NULL;
        } else {
            fprintf(stderr, "[totk-vi] Vulkan unavailable; using software framebuffer\n");
        }
    }
    fprintf(stderr, "[totk-vi] host native window %ux%u format=%#x layer=%llu backend=%s buffer=%p/%zu\n",
            width, height, w->format, (unsigned long long)layer_id,
            w->gfx_target ? "vulkan" : "software", (void *)w->buffer, w->buffer_size);
    host_window_register(w);
    return w;
}

void runtime_native_window_destroy(void *p) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return;
    host_window_unregister(w);
    if (w->gfx_target) gfx_backend_offscreen_destroy(w->gfx_target);
    if (w->surface_ok) gfx_backend_detach_android_surface();
    if (g_shell_window == w) g_shell_window = NULL;
    free(w->buffer);
    w->magic = 0;
    free(w);
}

int runtime_native_window_info(const void *p, uint32_t *width, uint32_t *height,
                               uint32_t *format, void **buffer, size_t *size) {
    const HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return 0;
    if (width) *width = w->width;
    if (height) *height = w->height;
    if (format) *format = w->format;
    if (buffer) *buffer = w->buffer;
    if (size) *size = w->buffer_size;
    return 1;
}

/* Attach the app shell's ANativeWindow to a host native window.
 *
 * `native_window` is an ANativeWindow* acquired from a Java Surface (or an
 * android_app window).  `host_window` is the HostNativeWindow* returned by
 * runtime_native_window_create(); when it is NULL a shell-owned window of
 * width x height is created and can be fetched with
 * runtime_native_window_shell_window().  `width`/`height` describe the surface
 * and are only used as the swapchain extent fallback; the host window keeps
 * its own offscreen size so the offscreen readback path is unaffected.
 *
 * Passing native_window == NULL detaches the surface: the host window keeps
 * its Vulkan offscreen target and present() falls back to it. */
void runtime_native_window_attach_android(void *native_window, void *host_window,
                                          uint32_t width, uint32_t height,
                                          uint32_t nvn_format) {
    HostNativeWindow *w = host_window;
    if (!w || w->magic != HOST_NW_MAGIC) {
        if (!native_window) {
            /* Detach with no host window: still release the global surface so
             * a dead Activity cannot leave a stale VkSurfaceKHR behind. */
            gfx_backend_detach_android_surface();
            return;
        }
        if (g_shell_window) {
            runtime_native_window_destroy(g_shell_window);
            g_shell_window = NULL;
        }
        w = runtime_native_window_create(width ? width : 1280, height ? height : 720,
                                         nvn_format ? nvn_format : 0x25, 0x53484c4cu /* "SHLL" */);
        if (!w) {
            fprintf(stderr, "[totk-vi] ERROR Android surface attach: no host native window\n");
            return;
        }
        g_shell_window = w;
    }
    if (!native_window) {
        w->android = NULL;
        w->offscreen = 1;
        w->surface_ok = 0;
        gfx_backend_detach_android_surface();
        fprintf(stderr, "[totk-vi] Android surface detached host=%p; offscreen target retained\n",
                (void *)w);
        return;
    }
    w->android = native_window;
    w->offscreen = 0;
    w->surface_ok = 0;
    uint32_t sw = width ? width : w->width;
    uint32_t sh = height ? height : w->height;
    if (sw != w->width || sh != w->height)
        fprintf(stderr, "[totk-vi] surface %ux%u differs from host window %ux%u; "
                        "offscreen target stays %ux%u\n",
                sw, sh, w->width, w->height, w->width, w->height);
    if (gfx_backend_attach_android_surface(native_window, sw, sh,
                                           nvn_format ? nvn_format : w->format)) {
        w->surface_ok = 1;
        fprintf(stderr, "[totk-vi] Android surface attached anw=%p host=%p surface=%ux%u "
                        "format=%#x\n",
                native_window, (void *)w, sw, sh, nvn_format ? nvn_format : w->format);
    } else {
        w->android = NULL;
        w->offscreen = 1;
        fprintf(stderr, "[totk-vi] ERROR Android surface attach failed; keeping offscreen target\n");
    }
}

/* Host window implicitly created by the attach call above (NULL if the caller
 * supplied its own). */
void *runtime_native_window_shell_window(void) { return g_shell_window; }

int runtime_native_window_surface_ready(const void *p) {
    const HostNativeWindow *w = p;
    return w && w->magic == HOST_NW_MAGIC && w->surface_ok;
}

unsigned runtime_native_window_present_count(const void *p) {
    const HostNativeWindow *w = p;
    return w && w->magic == HOST_NW_MAGIC ? w->presents : 0;
}

unsigned runtime_native_window_surface_present_count(const void *p) {
    const HostNativeWindow *w = p;
    return w && w->magic == HOST_NW_MAGIC ? w->surface_presents : 0;
}

void *runtime_native_window_android(const void *p) {
    const HostNativeWindow *w = p;
    return w && w->magic == HOST_NW_MAGIC ? w->android : NULL;
}

/* A CPU clear is the smallest observable "frame" for the offscreen milestone.
 * The real renderer will replace this with a Vulkan/GL target; keeping it here
 * means present can be validated without pretending the GPU executed. */
int runtime_native_window_clear(void *p, uint32_t rgba) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return 0;
    w->clear[0] = ((rgba >> 0) & 0xff) / 255.0f;
    w->clear[1] = ((rgba >> 8) & 0xff) / 255.0f;
    w->clear[2] = ((rgba >> 16) & 0xff) / 255.0f;
    w->clear[3] = ((rgba >> 24) & 0xff) / 255.0f;
    if (w->gfx_target) {
        if (!gfx_backend_offscreen_clear(w->gfx_target, w->clear[0], w->clear[1],
                                         w->clear[2], w->clear[3]))
            return 0;
        size_t size = 0;
        const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, &size);
        if (src) memcpy(w->buffer, src, size < w->buffer_size ? size : w->buffer_size);
        return 1;
    }
    uint32_t *px = (uint32_t *)w->buffer;
    size_t n = w->buffer_size / 4;
    for (size_t i = 0; i < n; i++) px[i] = rgba;
    return 1;
}

int runtime_native_window_present(void *p) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return 0;
    if (w->surface_ok && gfx_backend_surface_ready()) {
        if (!gfx_backend_surface_present(w->clear[0], w->clear[1], w->clear[2], w->clear[3]))
            return 0;
        w->presents++;
        w->surface_presents++;
        return 1;
    }
    if (w->gfx_target) {
        if (!gfx_backend_offscreen_present(w->gfx_target)) return 0;
        w->presents++;
        return 1;
    }
    fprintf(stderr, "[totk-vi] present software %ux%u\n", w->width, w->height);
    w->presents++;
    return 1;
}

const char *runtime_native_window_backend(const void *p) {
    const HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return "none";
    return w->gfx_target ? gfx_backend_name() : "software";
}

int runtime_native_window_draw_triangle_pipeline(void *p, void *pipeline) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !w->gfx_target || !pipeline) return 0;
    if (!gfx_backend_triangle_draw((GfxTrianglePipeline *)pipeline, w->gfx_target,
                                   0.05f, 0.08f, 0.12f, 1.0f))
        return 0;
    const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, NULL);
    if (src) memcpy(w->buffer, src, w->buffer_size);
    return 1;
}

int runtime_native_window_draw_triangle(void *p) {
    return runtime_native_window_draw_triangle_pipeline(p, gfx_backend_builtin_triangle_pipeline());
}

int runtime_native_window_draw_reflected_pipeline(void *p, void *pipeline) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !w->gfx_target || !pipeline) return 0;
    if (!gfx_backend_reflected_draw((GfxReflectedPipeline *)pipeline, w->gfx_target,
                                    0.05f, 0.08f, 0.12f, 1.0f))
        return 0;
    const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, NULL);
    if (src) memcpy(w->buffer, src, w->buffer_size);
    return 1;
}

int runtime_native_window_execute_reflected_ex(void *p, void *pipeline,
                                               const GfxCmd *cmds, uint32_t count,
                                               const float default_clear[4],
                                               const GfxVertexStream *streams, uint32_t nstreams);

int runtime_native_window_execute_reflected(void *p, void *pipeline,
                                            const GfxCmd *cmds, uint32_t count,
                                            const float default_clear[4]) {
    return runtime_native_window_execute_reflected_ex(p, pipeline, cmds, count, default_clear,
                                                      NULL, 0);
}

int runtime_native_window_execute_reflected_ex(void *p, void *pipeline,
                                               const GfxCmd *cmds, uint32_t count,
                                               const float default_clear[4],
                                               const GfxVertexStream *streams, uint32_t nstreams) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !w->gfx_target || !pipeline) return 0;
    if (!gfx_backend_reflected_execute_ex((GfxReflectedPipeline *)pipeline, w->gfx_target,
                                          cmds, count, default_clear, streams, nstreams))
        return 0;
    const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, NULL);
    if (src) memcpy(w->buffer, src, w->buffer_size);
    return 1;
}

/* Clear-only frame used when a program has no cached pipeline yet.  The
 * command buffer translation is still honoured (clear colour), but no draw is
 * invented. */
int runtime_native_window_clear_rgba(void *p, const float rgba[4]) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !rgba) return 0;
    w->clear[0] = rgba[0]; w->clear[1] = rgba[1];
    w->clear[2] = rgba[2]; w->clear[3] = rgba[3];
    if (w->gfx_target) {
        if (!gfx_backend_offscreen_clear(w->gfx_target, rgba[0], rgba[1], rgba[2], rgba[3]))
            return 0;
        size_t size = 0;
        const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, &size);
        if (src) memcpy(w->buffer, src, size < w->buffer_size ? size : w->buffer_size);
        return 1;
    }
    uint32_t px = ((uint32_t)(rgba[0] * 255.0f + 0.5f) << 0) |
                  ((uint32_t)(rgba[1] * 255.0f + 0.5f) << 8) |
                  ((uint32_t)(rgba[2] * 255.0f + 0.5f) << 16) |
                  ((uint32_t)(rgba[3] * 255.0f + 0.5f) << 24);
    uint32_t *dst = (uint32_t *)w->buffer;
    size_t n = w->buffer_size / 4;
    for (size_t i = 0; i < n; i++) dst[i] = px;
    return 1;
}

int runtime_native_window_draw_textured_triangle(void *p, void *pipeline, void *texture) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !w->gfx_target || !pipeline || !texture) return 0;
    if (!gfx_backend_textured_draw((GfxTexturedPipeline *)pipeline, (GfxTexture *)texture,
                                   w->gfx_target, 0.05f, 0.08f, 0.12f, 1.0f))
        return 0;
    const uint8_t *src = gfx_backend_offscreen_pixels(w->gfx_target, NULL);
    if (src) memcpy(w->buffer, src, w->buffer_size);
    return 1;
}

int runtime_native_window_read_pixel(void *p, uint32_t x, uint32_t y, uint8_t out[4]) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return 0;
    if (w->gfx_target) return gfx_backend_offscreen_read_pixel(w->gfx_target, x, y, out);
    if (x >= w->width || y >= w->height || !out) return 0;
    memcpy(out, w->buffer + ((size_t)y * w->width + x) * 4, 4);
    return 1;
}

int runtime_native_window_screenshot(void *p, const char *path) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC || !path) return 0;
    if (w->gfx_target) return gfx_backend_offscreen_write_ppm(w->gfx_target, path);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%u %u\n255\n", w->width, w->height);
    for (uint32_t y = 0; y < w->height; y++)
        for (uint32_t x = 0; x < w->width; x++)
            fwrite(w->buffer + ((size_t)y * w->width + x) * 4, 1, 3, f);
    fclose(f);
    return 1;
}
