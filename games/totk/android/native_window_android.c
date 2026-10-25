/* Host NativeWindow abstraction for the Android CPU-only path.
 *
 * The real Horizon native window is an Android Parcel carrying a dispdrv binder
 * object.  This host has no binder/compositor, so Phase 2A uses an explicit
 * offscreen description instead: width, height, format and a host framebuffer.
 * When the Android app shell supplies a real ANativeWindow, `android` is set and
 * the same description is also used for the first on-screen presentation.
 */
#include "runtime.h"
#include <stdint.h>
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
    uint8_t *buffer;
    size_t buffer_size;
} HostNativeWindow;

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
    fprintf(stderr, "[totk-vi] host native window %ux%u format=%#x layer=%llu offscreen buffer=%p/%zu\n",
            width, height, w->format, (unsigned long long)layer_id, (void *)w->buffer, w->buffer_size);
    return w;
}

void runtime_native_window_destroy(void *p) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return;
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

void runtime_native_window_attach_android(void *p, void *android) {
    HostNativeWindow *w = p;
    if (!w || w->magic != HOST_NW_MAGIC) return;
    w->android = android;
    w->offscreen = android == NULL;
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
    uint32_t *px = (uint32_t *)w->buffer;
    size_t n = w->buffer_size / 4;
    for (size_t i = 0; i < n; i++) px[i] = rgba;
    return 1;
}
