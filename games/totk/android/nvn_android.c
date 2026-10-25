/* Minimal NVN interface for the Android host.
 *
 * The 1.0.0 boot path calls nvnBootstrapLoader("nvnDeviceInitialize") and
 * ("nvnDeviceGetProcAddress"), then nvnDeviceGetInteger for the NVN ABI
 * version (55.15 on this image) before it creates the SEAD graphics device.
 * This file provides the real bootstrap/proc-address contract and a device
 * object with the X1 constants; drawable/queue/window objects are still stubs,
 * so the next missing call is visible instead of a fake success. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t (*NvnFn)(void);

static uint32_t nvn_bool_false(void) { return 0; }
static uint64_t nvn_zero64(void) { return 0; }
static void nvn_nop(void) { }
static uint32_t nvn_ok(void) { return 1; }

/* Opaque NVN object storage.  The guest owns the memory; we only remember the
 * native window a builder was given so nvnWindowGetNativeWindow can return it.
 * No field layouts beyond the builder/window pointer identity are needed for
 * the Phase 1 initialization path. */
typedef struct { void *key; void *native; } NvnWinSlot;
static NvnWinSlot nvn_windows[64];
static uint8_t nvn_default_native[0x40];
static void nvn_win_set(void *key, void *native) {
    for (unsigned i = 0; i < 64; i++) {
        if (nvn_windows[i].key == key || !nvn_windows[i].key) {
            nvn_windows[i].key = key;
            nvn_windows[i].native = native;
            return;
        }
    }
}
static void *nvn_win_get(void *key) {
    for (unsigned i = 0; i < 64; i++) if (nvn_windows[i].key == key) return nvn_windows[i].native;
    return NULL;
}
static void nvn_window_builder_set_native(void *builder, void *native) { nvn_win_set(builder, native); }
static void *nvn_window_builder_get_native(void *builder) { void *n = nvn_win_get(builder); return n ? n : nvn_default_native; }
static uint32_t nvn_window_initialize(void *window, void *builder) {
    void *native = nvn_win_get(builder);
    nvn_win_set(window, native ? native : nvn_default_native);
    return 1;
}
static void *nvn_window_get_native(void *window) {
    void *native = nvn_win_get(window);
    return native ? native : nvn_default_native;
}

/* Same values as the desktop NVN layer (nvn_DeviceConstantsNX.h, measured on
 * Tegra X1).  Entry 0/1 is the ABI version checked at main+0xe9d610. */
static const int nvn_devinfo[] = {
    [0x00] = 55, [0x01] = 15, [0x02] = 14, [0x03] = 65536, [0x04] = 256,
    [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16,
    [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4,
    [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256,
    [0x19] = 4096, [0x1A] = 131072, [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024,
    [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096,
    [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 262144, [0x2C] = 15,
    [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5,
    [0x32] = 1, [0x33] = 8, [0x34] = 8, [0x35] = 4, [0x36] = 32,
    [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1,
    [0x3C] = 4, [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048,
    [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1,
    [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16, [0x4E] = 1024,
    [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536,
    [0x53] = 256, [0x54] = 16384, [0x55] = 262144, [0x56] = 4096,
    [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096,
    [0x5C] = 16384, [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};

static void nvn_log_missing(const char *kind, const char *name) {
    static unsigned n;
    if (n++ < 40)
        fprintf(stderr, "[totk-nvn] %s %s -> stub\n", kind, name ? name : "(null)");
}

static uint32_t nvn_device_initialize(void *device, const void *builder) {
    (void)device; (void)builder;
    fprintf(stderr, "[totk-nvn] nvnDeviceInitialize\n");
    return 1;
}

static void nvn_device_get_integer(const void *device, int what, int *out) {
    (void)device;
    if (!out) return;
    if (what >= 0 && (size_t)what < sizeof nvn_devinfo / sizeof *nvn_devinfo)
        *out = nvn_devinfo[what];
    else
        *out = 0;
}

static void *nvn_lookup(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceInitialize")) return (void *)nvn_device_initialize;
    if (!strcmp(name, "nvnDeviceGetInteger")) return (void *)nvn_device_get_integer;
    if (!strcmp(name, "nvnDeviceBuilderSetDefaults") ||
        !strcmp(name, "nvnDeviceBuilderSetFlags") ||
        !strcmp(name, "nvnMemoryPoolBuilderSetDefaults") ||
        !strcmp(name, "nvnMemoryPoolBuilderSetDevice") ||
        !strcmp(name, "nvnMemoryPoolBuilderSetStorage") ||
        !strcmp(name, "nvnMemoryPoolBuilderSetFlags") ||
        !strcmp(name, "nvnBufferBuilderSetDefaults") ||
        !strcmp(name, "nvnBufferBuilderSetDevice") ||
        !strcmp(name, "nvnBufferBuilderSetStorage") ||
        !strcmp(name, "nvnTextureBuilderSetDefaults") ||
        !strcmp(name, "nvnTextureBuilderSetDevice") ||
        !strcmp(name, "nvnTextureBuilderSetStorage") ||
        !strcmp(name, "nvnTextureBuilderSetFlags") ||
        !strcmp(name, "nvnTextureBuilderSetImageType") ||
        !strcmp(name, "nvnTextureBuilderSetFormat") ||
        !strcmp(name, "nvnTextureBuilderSetSize1D") ||
        !strcmp(name, "nvnTextureBuilderSetSize2D") ||
        !strcmp(name, "nvnTextureBuilderSetSize3D") ||
        !strcmp(name, "nvnTextureBuilderSetStride") ||
        !strcmp(name, "nvnTextureBuilderSetTileMode") ||
        !strcmp(name, "nvnTextureBuilderSetPackagedTextureLayout") ||
        !strcmp(name, "nvnSamplerBuilderSetDefaults") ||
        !strcmp(name, "nvnSamplerBuilderSetDevice") ||
        !strcmp(name, "nvnQueueBuilderSetDefaults") ||
        !strcmp(name, "nvnQueueBuilderSetDevice") ||
        !strcmp(name, "nvnQueueBuilderSetFlags") ||
        !strcmp(name, "nvnQueueBuilderSetCommandMemorySize") ||
        !strcmp(name, "nvnQueueBuilderSetComputeMemorySize") ||
        !strcmp(name, "nvnQueueBuilderSetControlMemorySize") ||
        !strcmp(name, "nvnQueueBuilderSetQueueMemory") ||
        !strcmp(name, "nvnQueueBuilderSetCommandFlushThreshold") ||
        !strcmp(name, "nvnQueueBuilderSetQueuePriority") ||
        !strcmp(name, "nvnWindowBuilderSetDefaults") ||
        !strcmp(name, "nvnWindowBuilderSetDevice") ||
        !strcmp(name, "nvnWindowBuilderSetNativeWindow") ||
        !strcmp(name, "nvnWindowBuilderSetTextures") ||
        !strcmp(name, "nvnWindowBuilderSetPresentInterval"))
        return !strcmp(name, "nvnWindowBuilderSetNativeWindow")
             ? (void *)nvn_window_builder_set_native : (void *)nvn_nop;
    if (!strcmp(name, "nvnWindowBuilderGetNativeWindow")) return (void *)nvn_window_builder_get_native;
    if (!strcmp(name, "nvnWindowGetNativeWindow")) return (void *)nvn_window_get_native;
    if (!strcmp(name, "nvnMemoryPoolInitialize") ||
        !strcmp(name, "nvnBufferInitialize") ||
        !strcmp(name, "nvnTextureInitialize") ||
        !strcmp(name, "nvnSamplerInitialize") ||
        !strcmp(name, "nvnProgramInitialize") ||
        !strcmp(name, "nvnProgramSetShaders") ||
        !strcmp(name, "nvnQueueInitialize") ||
        !strcmp(name, "nvnWindowInitialize"))
        return !strcmp(name, "nvnWindowInitialize") ? (void *)nvn_window_initialize : (void *)nvn_ok;
    if (!strcmp(name, "nvnDeviceFinalize") ||
        !strcmp(name, "nvnDeviceSetDebugLabel") ||
        !strcmp(name, "nvnDeviceSetIntermediateShaderCache") ||
        !strcmp(name, "nvnDeviceInstallDebugCallback") ||
        !strcmp(name, "nvnDeviceSetWindowOriginMode") ||
        !strcmp(name, "nvnDeviceSetDepthMode") ||
        !strcmp(name, "nvnDeviceApplyDeferredFinalizes"))
        return (void *)nvn_nop;
    if (!strcmp(name, "nvnDeviceGetCurrentTimestampInNanoseconds") ||
        !strcmp(name, "nvnDeviceGetTimestampInNanoseconds") ||
        !strcmp(name, "nvnDeviceGetTextureHandle") ||
        !strcmp(name, "nvnDeviceGetTexelFetchHandle") ||
        !strcmp(name, "nvnDeviceGetImageHandle") ||
        !strcmp(name, "nvnDeviceGetSeparateTextureHandle") ||
        !strcmp(name, "nvnDeviceGetSeparateSamplerHandle") ||
        !strcmp(name, "nvnDeviceGenerateDebugDomainId"))
        return (void *)nvn_zero64;
    if (!strcmp(name, "nvnDeviceRegisterFastClearColor") ||
        !strcmp(name, "nvnDeviceRegisterFastClearColori") ||
        !strcmp(name, "nvnDeviceRegisterFastClearColorui") ||
        !strcmp(name, "nvnDeviceRegisterFastClearDepth") ||
        !strcmp(name, "nvnDeviceIsExternalDebuggerAttached"))
        return (void *)nvn_bool_false;
    nvn_log_missing("proc", name);
    return (void *)nvn_zero64;   /* harmless for void/int-returning entries */
}

static void *nvn_device_get_proc_address(const void *device, const char *name) {
    (void)device;
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvn_device_get_proc_address;
    void *fn = nvn_lookup(name);
    if (!fn) return NULL;
    return fn;
}

void *runtime_nvn_bootstrap(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "nvnDeviceInitialize")) return (void *)nvn_device_initialize;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (void *)nvn_device_get_proc_address;
    void *fn = nvn_lookup(name);
    if (!fn) nvn_log_missing("bootstrap", name);
    return fn ? fn : (void *)nvn_zero64;
}
