/* NVN module: device. */
#include "nvn_internal.h"

/* ------------------------------------------------------------------ */
/* Device                                                              */

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
    [0x60] = 1, [0x61] = 1,
};

void nvnDeviceBuilderSetDefaults(dev_builder_t *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
}

void nvnDeviceBuilderSetFlags(dev_builder_t *b, int flags) {
    if (b) b->flags = (uint32_t)flags;
}

uint32_t nvnDeviceBuilderGetFlags(const dev_builder_t *b) {
    return b ? b->flags : 0;
}

uint32_t nvnDeviceInitialize(nvn_dev_t *d, const dev_builder_t *b) { nvn_api("nvnDeviceInitialize");
    nvn_install_host_crash_late();
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->magic = NVN_MAGIC('D', 'E', 'V', 0);
    d->flags = b ? b->flags : 0;
    d->origin = 0;
    d->depth_mode = 0;
    fprintf(stderr, "[totk-nvn] device initialized flags=%#x (CPU-only, no GPU submit)\n", d->flags);
    return 1;
}

void nvnDeviceFinalize(nvn_dev_t *d) { if (d) { nvn_host_rt_clear(); d->magic = 0; } }

void nvnDeviceSetDebugLabel(nvn_dev_t *d, const char *label) { (void)d; (void)label; }

void nvnDeviceGetInteger(const nvn_dev_t *d, int what, int *out) { nvn_api("nvnDeviceGetInteger");
    if (!d || !nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceGetInteger") || !out) return;
    if (what >= 0 && (size_t)what < sizeof nvn_devinfo / sizeof *nvn_devinfo) *out = nvn_devinfo[what];
    else *out = 0;
    if (nvn_trace_save_ui()) {
        extern const char *runtime_fs_last_op(void);
        fprintf(stderr, "[totk-save-ui] GetInteger what=%d -> %d fs=\"%s\"\n",
                what, *out, runtime_fs_last_op());
    }
}

uint64_t nvnDeviceGetCurrentTimestampInNanoseconds(const nvn_dev_t *d) {
    (void)d;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t nvnDeviceGetTimestampInNanoseconds(const nvn_dev_t *d, const void *counter) {
    (void)d; (void)counter;
    /* Guest timing (loading/fade progress) must advance; returning 0 made a
     * timestamp-based wait look permanently expired. */
    return runtime_now_ns();
}

/* NVN texture handles pack the texture pool id in the low 20 bits and the
 * sampler pool id in bits 20..31 (see TextureHandle.Unpack* in the translator).
 * Separate handles are the raw pool ids; the combined handle below is what the
 * game writes into the shader's c[2] handle slots. */
uint64_t nvnDeviceGetTextureHandle(const nvn_dev_t *d, int index, int sampler) {
    (void)d;
    return ((uint64_t)((uint32_t)sampler & 0xfffu) << 20) | ((uint32_t)index & 0xfffffu);
}

uint64_t nvnDeviceGetTexelFetchHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}

uint64_t nvnDeviceGetImageHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}

uint64_t nvnDeviceGetSeparateTextureHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffffu;
}

uint64_t nvnDeviceGetSeparateSamplerHandle(const nvn_dev_t *d, int index) {
    (void)d;
    return (uint32_t)index & 0xfffu;
}

uint32_t nvnDeviceIsExternalDebuggerAttached(const nvn_dev_t *d) { (void)d; return 0; }

void nvnDeviceInstallDebugCallback(nvn_dev_t *d, const void *cb, void *data, uint32_t enable) {
    if (!d) return;
    d->dbg_cb = enable ? (void *)cb : NULL;
    d->dbg_data = data;
}

int nvnDeviceGenerateDebugDomainId(const nvn_dev_t *d, const char *name) {
    (void)d; (void)name; return 1;
}

void nvnDeviceSetWindowOriginMode(nvn_dev_t *d, int mode) {
    if (d && nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceSetWindowOriginMode")) {
        d->origin = mode;
        if (nvn_trace_frame())
            fprintf(stderr, "[totk-frame] window origin mode=%d (%s)\n", mode,
                    mode == 1 ? "upper-left" : "lower-left");
    }
}

int nvnDeviceGetWindowOriginMode(const nvn_dev_t *d) { return d ? d->origin : 0; }

void nvnDeviceSetDepthMode(nvn_dev_t *d, int mode) {
    if (d && nvn_obj(d, NVN_MAGIC('D', 'E', 'V', 0), "nvnDeviceSetDepthMode")) {
        d->depth_mode = mode;
        if (nvn_trace_frame())
            fprintf(stderr, "[totk-frame] depth mode=%d (%s)\n", mode,
                    mode == 1 ? "near-is-zero" : "near-is-minus-w");
    }
}

int nvnDeviceGetDepthMode(const nvn_dev_t *d) { return d ? d->depth_mode : 0; }

uint32_t nvnDeviceRegisterFastClearColor(nvn_dev_t *d, const float *c, int f) { (void)d; (void)c; (void)f; return 1; }

uint32_t nvnDeviceRegisterFastClearColori(nvn_dev_t *d, const int *c, int f) { (void)d; (void)c; (void)f; return 1; }

uint32_t nvnDeviceRegisterFastClearColorui(nvn_dev_t *d, const uint32_t *c, int f) { (void)d; (void)c; (void)f; return 1; }

uint32_t nvnDeviceRegisterFastClearDepth(nvn_dev_t *d, float v) { (void)d; (void)v; return 1; }

void nvnDeviceSetIntermediateShaderCache(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceApplyDeferredFinalizes(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceFinalizeCommandHandle(nvn_dev_t *d, uint64_t h) { (void)d; (void)h; }

void nvnDeviceWalkDebugDatabase(const nvn_dev_t *d, int t, void *cb, void *u) { (void)d; (void)t; (void)cb; (void)u; }

void nvnDeviceSetAutomaticCheckpointsEnable(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceSetAutomaticCheckpointsMask(nvn_dev_t *d, uint64_t v) { (void)d; (void)v; }

void nvnDeviceSetAutomaticCheckpointsSamplingInterval(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceSetAutomaticCheckpointsByDebugGroup(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceSetCheckpointCallStacksEnable(nvn_dev_t *d, int v) { (void)d; (void)v; }

void nvnDeviceSetCheckpointMaxCallStacksDepth(nvn_dev_t *d, int v) { (void)d; (void)v; }
