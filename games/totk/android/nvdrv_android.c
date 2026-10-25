/* Minimal nvdrv host service for the Android port.
 *
 * The guest SDK talks to "nvdrv" through the Horizon INvDrvServices protocol:
 * Initialize, SetAruid, SetGraphicsFirmwareMemoryMarginEnabled, Open, Ioctl3,
 * Close.  The host has no Tegra GPU; this file implements the object and
 * buffer semantics the SDK's NvRm* layer needs so that later graphics code
 * gets real fds, nvmap handles and mapping addresses instead of a fake
 * success.  Command/ioctl shapes follow the public Horizon protocol
 * (switchbrew "NV services"); every unknown command returns an error and is
 * logged.
 */
#include "runtime.h"
#include "ipc.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* INvDrvServices command ids. */
enum {
    NVDRV_OPEN = 0,
    NVDRV_IOCTL = 1,
    NVDRV_CLOSE = 2,
    NVDRV_INITIALIZE = 3,
    NVDRV_QUERY_EVENT = 4,
    NVDRV_MAP_SHARED_MEM = 5,
    NVDRV_GET_STATUS = 6,
    NVDRV_SET_ARUID_WITHOUT_CHECK = 7,
    NVDRV_SET_ARUID = 8,
    NVDRV_DUMP_STATUS = 9,
    NVDRV_INITIALIZE_DEVTOOLS = 10,
    NVDRV_IOCTL2 = 11,
    NVDRV_IOCTL3 = 12,
    NVDRV_SET_GRAPHICS_FIRMWARE_MEMORY_MARGIN = 13,
};

#define NVDRV_MAX_FD 64
#define NVDRV_MAX_MAP 128

typedef struct {
    char path[64];
    int used;
    int is_nvmap;
    int is_ctrl;
    int is_gpu;
    int is_disp;
    int is_sched;
    int is_nvdec;
} nvdrv_fd;

typedef struct {
    uint32_t handle;
    uint32_t id;
    uint64_t size;
    uint32_t flags;
    uint32_t align;
    uint8_t kind;
    uint64_t addr;             /* guest-visible mapping, 0 when not mapped */
    void *host;                /* backing guest-visible allocation */
    int guest;
    int used;
} nvdrv_map;

static nvdrv_fd nvdrv_fds[NVDRV_MAX_FD];
static nvdrv_map nvdrv_maps[NVDRV_MAX_MAP];
static int nvdrv_trace = -1;
static uint32_t nvdrv_next_fd = 0x10;
static uint32_t nvdrv_next_handle = 1;
static uint32_t nvdrv_next_map_id = 1;

/* nvmap backing memory must be guest-visible.  Reserve a dedicated window
 * below the 39-bit Horizon limit; every allocation is committed in the guest
 * memory accounting so QueryMemory agrees with the pointer the SDK receives. */
#define NVDRV_GUEST_AT 0x500000000ull
#define NVDRV_GUEST_SIZE (1ull << 30)
static uint8_t *nvdrv_guest_area;
static size_t nvdrv_guest_used;
extern void runtime_guest_commit(uintptr_t base, uintptr_t size);
extern void runtime_guest_map_extra(uintptr_t base, uintptr_t size);

static int nvdrv_guest_init(void) {
    if (nvdrv_guest_area) return 1;
    void *p = mmap((void *)(uintptr_t)NVDRV_GUEST_AT, NVDRV_GUEST_SIZE,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
#ifdef MAP_FIXED_NOREPLACE
                   MAP_FIXED_NOREPLACE
#else
                   MAP_FIXED
#endif
                   , -1, 0);
    if (p == MAP_FAILED) {
        p = mmap(NULL, NVDRV_GUEST_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return 0;
        fprintf(stderr, "[totk-nvdrv] WARNING: guest nvmap window fell back to host address %p\n", p);
    }
    nvdrv_guest_area = p;
    nvdrv_guest_used = 0;
    if ((uintptr_t)p >= 0x100000000ull && (uintptr_t)p < (1ull << 39))
        runtime_guest_map_extra((uintptr_t)p, NVDRV_GUEST_SIZE);
    fprintf(stderr, "[totk-nvdrv] guest nvmap window %p+%#llx\n", (void *)p,
            (unsigned long long)NVDRV_GUEST_SIZE);
    return 1;
}
static void *nvdrv_guest_alloc(size_t size, size_t align) {
    if (!size) return NULL;
    if (align < 0x1000) align = 0x1000;
    /* The SDK passes the alignment straight from the nvmap ioctl; round it to
     * a power of two so the mask below is valid. */
    size_t pow2 = 0x1000;
    while (pow2 < align && pow2 < (size_t)NVDRV_GUEST_SIZE) pow2 <<= 1;
    align = pow2;
    if (!nvdrv_guest_init()) return NULL;
    size_t off = (nvdrv_guest_used + align - 1) & ~(align - 1);
    if (off + size < off || off + size > NVDRV_GUEST_SIZE) return NULL;
    void *p = nvdrv_guest_area + off;
    nvdrv_guest_used = off + size;
    runtime_guest_commit((uintptr_t)p, size);
    memset(p, 0, size);
    return p;
}

static int trace_enabled(void) {
    int v = __atomic_load_n(&nvdrv_trace, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("TOTK_ANDROID_TRACE_NVDRV");
        v = e && *e && strcmp(e, "0") ? 1 : 0;
        __atomic_store_n(&nvdrv_trace, v, __ATOMIC_RELAXED);
    }
    return v;
}

static void nvdrv_log(const char *what, uint32_t cmd, const IpcRequest *rq) {
    if (!trace_enabled()) return;
    fprintf(stderr, "[totk-nvdrv] %s cmd=%u type=%u data=%u nx=%u na=%u nb=%u nw=%u nc=%u copy=%u move=%u\n",
            what, cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nw, rq->nc,
            rq->ncopy, rq->nmove);
    if (rq->data_size) {
        fprintf(stderr, "[totk-nvdrv]   data:");
        for (unsigned i = 0; i * 4 + 4 <= rq->data_size && i < 16; i++)
            fprintf(stderr, " %08x", IPC_ARG(rq, uint32_t, i * 4));
        fputc('\n', stderr);
    }
    for (unsigned i = 0; i < rq->nx; i++)
        fprintf(stderr, "[totk-nvdrv]   x%u type=%#x addr=%#llx size=%#llx\n",
                i, rq->x[i].index, (unsigned long long)rq->x[i].addr,
                (unsigned long long)rq->x[i].size);
    for (unsigned i = 0; i < rq->ncopy; i++)
        fprintf(stderr, "[totk-nvdrv]   copy%u handle=%#x\n", i, rq->copy[i]);
    for (unsigned i = 0; i < rq->nmove; i++)
        fprintf(stderr, "[totk-nvdrv]   move%u handle=%#x\n", i, rq->move[i]);
}

static uint32_t nvdrv_bad(uint32_t cmd) {
    fprintf(stderr, "[totk-nvdrv] ERROR unsupported nvdrv command %u\n", cmd);
    return 0x1a8; /* INVALID_PARAMETER-ish; callers surface a real failure */
}

static nvdrv_fd *fd_get(uint32_t fd) {
    if (fd >= NVDRV_MAX_FD || !nvdrv_fds[fd].used) return NULL;
    return &nvdrv_fds[fd];
}
static nvdrv_map *map_find(uint32_t handle) {
    for (unsigned i = 0; i < NVDRV_MAX_MAP; i++)
        if (nvdrv_maps[i].used && nvdrv_maps[i].handle == handle) return &nvdrv_maps[i];
    return NULL;
}
static nvdrv_map *map_find_id(uint32_t id) {
    for (unsigned i = 0; i < NVDRV_MAX_MAP; i++)
        if (nvdrv_maps[i].used && nvdrv_maps[i].id == id) return &nvdrv_maps[i];
    return NULL;
}
static nvdrv_map *map_new(uint64_t size, uint64_t addr, uint32_t flags, uint32_t align,
                          uint8_t kind, int host_alloc) {
    for (unsigned i = 0; i < NVDRV_MAX_MAP; i++) {
        nvdrv_map *m = &nvdrv_maps[i];
        if (m->used) continue;
        memset(m, 0, sizeof *m);
        m->size = size;
        m->addr = addr;
        m->flags = flags;
        m->align = align;
        m->kind = kind;
        m->host = host_alloc ? nvdrv_guest_alloc((size_t)size, align ? align : 0x1000) : NULL;
        if (host_alloc && !m->host) return NULL;
        m->guest = host_alloc != 0;
        m->handle = nvdrv_next_handle++;
        if (!m->handle) m->handle = nvdrv_next_handle++;
        m->id = nvdrv_next_map_id++;
        if (!m->id) m->id = nvdrv_next_map_id++;
        m->used = 1;
        return m;
    }
    return NULL;
}
static void map_free(nvdrv_map *m) {
    if (!m || !m->used) return;
    /* The guest window is a bump allocator: a block cannot be handed back to
     * the OS, but zero it so a later object never observes freed contents. */
    if (m->host && m->guest && m->size) memset(m->host, 0, (size_t)m->size);
    memset(m, 0, sizeof *m);
}

/* Nvmap ioctl codes (switchbrew NV services). */
enum {
    NVMAP_IOC_CREATE = 0xC0080101,
    NVMAP_IOC_CLAIM = 0x00000102,
    NVMAP_IOC_FROM_ID = 0xC0080103,
    NVMAP_IOC_ALLOC = 0xC0200104,
    NVMAP_IOC_FREE = 0xC0180105,
    NVMAP_IOC_MMAP = 0xC0280106,
    NVMAP_IOC_WRITE = 0xC0280107,
    NVMAP_IOC_READ = 0xC0280108,
    NVMAP_IOC_PARAM = 0xC00C0109,
    NVMAP_IOC_PIN_MULT = 0xC010010A,
    NVMAP_IOC_UNPIN_MULT = 0xC010010B,
    NVMAP_IOC_CACHE = 0xC008010C,
    NVMAP_IOC_GET_IVC_ID = 0xC004010D,
    NVMAP_IOC_GET_ID = 0xC008010E,
    NVMAP_IOC_FROM_IVC_ID = 0xC004010F,
    NVMAP_IOC_SET_ALLOCATION_TAG_LABEL = 0x40040110,
    NVMAP_IOC_RESERVE = 0x00000111,
    NVMAP_IOC_EXPORT_FOR_ARUID = 0x40100112,
    NVMAP_IOC_IS_OWNED_BY_ARUID = 0x40100113,
    NVMAP_IOC_REMOVE_EXPORT_FOR_ARUID = 0x40100114,
};

/* nvhost-ctrl ioctl codes. */
enum {
    NVHOST_IOCTL_CTRL_SYNCPT_READ = 0xC0080014,
    NVHOST_IOCTL_CTRL_SYNCPT_INCR = 0x40040015,
    NVHOST_IOCTL_CTRL_SYNCPT_WAIT = 0xC00C0016,
    NVHOST_IOCTL_CTRL_MODULE_MUTEX = 0x40080017,
    NVHOST_IOCTL_CTRL_MODULE_REGRDWR = 0xC0180018,
    NVHOST_IOCTL_CTRL_SYNCPT_WAITEX = 0xC0100019,
    NVHOST_IOCTL_CTRL_SYNCPT_READ_MAX = 0xC008001A,
    NVHOST_IOCTL_CTRL_GET_CONFIG = 0xC183001B,
    NVHOST_IOCTL_CTRL_SYNCPT_CLEAR_EVENT_WAIT = 0xC004001C,
    NVHOST_IOCTL_CTRL_SYNCPT_WAIT_EVENT = 0xC010001D,
    NVHOST_IOCTL_CTRL_SYNCPT_WAIT_EVENT_EX = 0xC010001E,
    NVHOST_IOCTL_CTRL_SYNCPT_ALLOC_EVENT = 0xC004001F,
    NVHOST_IOCTL_CTRL_SYNCPT_FREE_EVENT = 0xC0040020,
    NVHOST_IOCTL_CTRL_SYNCPT_FREE_EVENT_BATCH = 0x40080021,
    NVHOST_IOCTL_CTRL_SYNCPT_GET_SHIFT = 0xC0040022,
};

static uint32_t syncpt_value[64];
static int syncpt_used[64];

static uint32_t host_ioctl_nvmap(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
                                uint8_t *out, uint32_t out_size) {
    (void)fd;
    switch (code) {
    case NVMAP_IOC_CREATE: {
        if (in_size < 4 || out_size < 8) return 0x1a8;
        uint32_t size;
        memcpy(&size, in, 4);
        nvdrv_map *m = map_new(size, 0, 0, 0x1000, 0, 1);
        if (!m) return 0x1a8;
        /* The ioctl structure is { size, handle }; the handle is the output. */
        memcpy(out, &size, 4);
        memcpy(out + 4, &m->handle, 4);
        if (trace_enabled()) fprintf(stderr, "[totk-nvdrv] nvmap create size=%#x handle=%#x\n", size, m->handle);
        return 0;
    }
    case NVMAP_IOC_FROM_ID: {
        if (in_size < 4 || out_size < 8) return 0x1a8;
        uint32_t id;
        memcpy(&id, in, 4);
        nvdrv_map *m = map_find_id(id);
        if (!m) return 0x1a8;
        memcpy(out, &id, 4);
        memcpy(out + 4, &m->handle, 4);
        return 0;
    }
    case NVMAP_IOC_ALLOC: {
        if (in_size < 32 || out_size < 32) return 0x1a8;
        uint32_t handle, heapmask, flags, align;
        uint8_t kind;
        uint64_t addr;
        memcpy(&handle, in + 0, 4);
        memcpy(&heapmask, in + 4, 4);
        memcpy(&flags, in + 8, 4);
        memcpy(&align, in + 12, 4);
        kind = in[16];
        memcpy(&addr, in + 24, 8);
        (void)heapmask;
        nvdrv_map *m = map_find(handle);
        if (!m) return 0x1a8;
        if (m->size == 0) m->size = align ? align : 0x1000;
        /* The backing block is a slice of the shared guest window; CREATE owns
         * it and nothing here may munmap it.  ALLOC only decides which address
         * the SDK gets back from MMAP. */
        if (!m->host && !addr) {
            m->host = nvdrv_guest_alloc((size_t)m->size, align ? align : 0x1000);
            m->guest = m->host != NULL;
        }
        m->addr = m->host ? (uintptr_t)m->host : addr;
        m->flags = flags;
        m->align = align;
        m->kind = kind;
        if (!m->addr && !(flags & 1)) return 0x1a8;
        memcpy(out, in, out_size < 32 ? out_size : 32);
        if (trace_enabled())
            fprintf(stderr, "[totk-nvdrv] nvmap alloc handle=%#x size=%#llx addr=%#llx flags=%#x kind=%u\n",
                    handle, (unsigned long long)m->size, (unsigned long long)addr, flags, kind);
        return 0;
    }
    case NVMAP_IOC_FREE: {
        if (in_size < 24 || out_size < 24) return 0x1a8;
        uint32_t handle;
        memcpy(&handle, in + 0, 4);
        nvdrv_map *m = map_find(handle);
        if (!m) return 0x1a8;
        uint64_t address = m->addr;
        uint32_t size = (uint32_t)m->size, flags = m->flags & 2;
        memset(out, 0, 24);
        memcpy(out + 0, &handle, 4);
        memcpy(out + 8, &address, 8);
        memcpy(out + 16, &size, 4);
        memcpy(out + 20, &flags, 4);
        map_free(m);
        return 0;
    }
    case NVMAP_IOC_MMAP: {
        if (in_size < 40 || out_size < 40) return 0x1a8;
        uint32_t handle;
        memcpy(&handle, in, 4);
        nvdrv_map *m = map_find(handle);
        if (!m) return 0x1a8;
        if (!m->addr) m->addr = (uintptr_t)m->host;
        if (!m->addr) {
            fprintf(stderr, "[totk-nvdrv] ERROR nvmap mmap handle=%#x has no backing\n", handle);
            return 0x1a8;
        }
        memcpy(out, in, out_size < 40 ? out_size : 40);
        if (out_size >= 40) memcpy(out + 8, &m->addr, 8);
        return 0;
    }
    case NVMAP_IOC_PARAM: {
        if (in_size < 12 || out_size < 12) return 0x1a8;
        uint32_t handle, param, result = 0;
        memcpy(&handle, in, 4);
        memcpy(&param, in + 4, 4);
        nvdrv_map *m = map_find(handle);
        if (!m) return 0x1a8;
        switch (param) {
        case 1: result = (uint32_t)m->size; break;      /* SIZE */
        case 2: result = m->align ? m->align : 0x1000; break; /* ALIGNMENT */
        case 4: result = 0x40000000; break;             /* HEAP */
        case 5: result = m->kind; break;                /* KIND */
        default: result = 0; break;
        }
        memcpy(out, in, 8);
        memcpy(out + 8, &result, 4);
        return 0;
    }
    case NVMAP_IOC_GET_ID: {
        if (in_size < 8 || out_size < 8) return 0x1a8;
        uint32_t handle;
        memcpy(&handle, in + 4, 4);
        nvdrv_map *m = map_find(handle);
        if (!m) return 0x1a8;
        memcpy(out, &m->id, 4);
        memcpy(out + 4, &handle, 4);
        return 0;
    }
    case NVMAP_IOC_CACHE:
    case NVMAP_IOC_SET_ALLOCATION_TAG_LABEL:
    case NVMAP_IOC_EXPORT_FOR_ARUID:
    case NVMAP_IOC_IS_OWNED_BY_ARUID:
    case NVMAP_IOC_REMOVE_EXPORT_FOR_ARUID:
        if (out_size) memcpy(out, in, out_size < in_size ? out_size : in_size);
        return 0;
    default:
        fprintf(stderr, "[totk-nvdrv] ERROR nvmap ioctl %#x not implemented\n", code);
        return 0x1a8;
    }
}

static uint32_t host_ioctl_ctrl(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
                                uint8_t *out, uint32_t out_size) {
    (void)fd;
    switch (code) {
    case NVHOST_IOCTL_CTRL_SYNCPT_READ:
    case NVHOST_IOCTL_CTRL_SYNCPT_READ_MAX: {
        if (in_size < 4 || out_size < 8) return 0x1a8;
        uint32_t id;
        memcpy(&id, in, 4);
        uint32_t value = id < 64 ? syncpt_value[id] : 0;
        memcpy(out, in, 4);
        memcpy(out + 4, &value, 4);
        return 0;
    }
    case NVHOST_IOCTL_CTRL_SYNCPT_INCR: {
        if (in_size < 4) return 0x1a8;
        uint32_t id;
        memcpy(&id, in, 4);
        if (id < 64) { syncpt_used[id] = 1; syncpt_value[id]++; }
        return 0;
    }
    case NVHOST_IOCTL_CTRL_SYNCPT_WAIT:
    case NVHOST_IOCTL_CTRL_SYNCPT_WAITEX:
    case NVHOST_IOCTL_CTRL_SYNCPT_WAIT_EVENT:
    case NVHOST_IOCTL_CTRL_SYNCPT_WAIT_EVENT_EX: {
        /* The host has no GPU engine; the syncpoint has already reached the
         * requested threshold because submissions are CPU-side and complete
         * immediately.  Fill the output value, never claim a fake timeout. */
        if (in_size < 8) return 0x1a8;
        uint32_t id, threshold;
        memcpy(&id, in, 4);
        memcpy(&threshold, in + 4, 4);
        if (id < 64 && syncpt_value[id] < threshold) syncpt_value[id] = threshold;
        if (out_size >= 12) {
            memcpy(out, in, out_size < in_size ? out_size : in_size);
            uint32_t value = id < 64 ? syncpt_value[id] : threshold;
            memcpy(out + 8, &value, 4);
        }
        return 0;
    }
    case NVHOST_IOCTL_CTRL_SYNCPT_ALLOC_EVENT:
    case NVHOST_IOCTL_CTRL_SYNCPT_FREE_EVENT:
    case NVHOST_IOCTL_CTRL_SYNCPT_CLEAR_EVENT_WAIT:
    case NVHOST_IOCTL_CTRL_SYNCPT_GET_SHIFT: {
        if (out_size) {
            uint32_t v = 0;
            memcpy(out, &v, out_size < 4 ? out_size : 4);
        }
        return 0;
    }
    case NVHOST_IOCTL_CTRL_MODULE_MUTEX:
        return 0;
    case NVHOST_IOCTL_CTRL_GET_CONFIG: {
        /* struct { char name[0x41]; char key[0x41]; char value[0x101]; } */
        if (out_size >= 0x183) {
            memset(out, 0, 0x183);
            memcpy(out + 0x82, "0", 2);
        }
        return 0;
    }
    default:
        fprintf(stderr, "[totk-nvdrv] ERROR nvhost-ctrl ioctl %#x not implemented\n", code);
        return 0x1a8;
    }
}

static uint32_t host_ioctl_generic(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
                                   uint8_t *out, uint32_t out_size) {
    (void)in; (void)in_size; (void)out; (void)out_size;
    fprintf(stderr, "[totk-nvdrv] ERROR ioctl %#x on %s not implemented\n",
            code, fd ? fd->path : "?");
    return 0x1a8;
}

static uint32_t nvdrv_ioctl(uint32_t fd, uint32_t code, uint8_t *in, uint32_t in_size,
                            uint8_t *out, uint32_t out_size) {
    nvdrv_fd *d = fd_get(fd);
    if (!d) return 0x1a8;
    if (d->is_nvmap) return host_ioctl_nvmap(d, code, in, in_size, out, out_size);
    if (d->is_ctrl) return host_ioctl_ctrl(d, code, in, in_size, out, out_size);
    return host_ioctl_generic(d, code, in, in_size, out, out_size);
}

static uint32_t nvdrv_open_path(const char *path) {
    uint32_t fd = nvdrv_next_fd++;
    if (fd >= NVDRV_MAX_FD) {
        nvdrv_next_fd = 0x10;
        fd = nvdrv_next_fd++;
    }
    for (unsigned tries = 0; tries < NVDRV_MAX_FD; tries++) {
        if (!nvdrv_fds[fd].used) break;
        fd = nvdrv_next_fd++;
        if (fd >= NVDRV_MAX_FD) { nvdrv_next_fd = 0x10; fd = nvdrv_next_fd++; }
    }
    if (fd >= NVDRV_MAX_FD || nvdrv_fds[fd].used) return 0;
    nvdrv_fd *d = &nvdrv_fds[fd];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    d->is_nvmap = strstr(path, "/dev/nvmap") != NULL;
    d->is_ctrl = strstr(path, "nvhost-ctrl") != NULL && !strstr(path, "ctrl-gpu");
    d->is_gpu = strstr(path, "nvhost-gpu") != NULL || strstr(path, "ctrl-gpu") != NULL ||
                strstr(path, "as-gpu") != NULL || strstr(path, "tsg-gpu") != NULL;
    d->is_disp = strstr(path, "nvdisp") != NULL;
    d->is_sched = strstr(path, "nvsched") != NULL;
    d->is_nvdec = strstr(path, "nvdec") != NULL || strstr(path, "nvjpg") != NULL;
    d->used = 1;
    if (trace_enabled()) fprintf(stderr, "[totk-nvdrv] open %s -> fd %#x\n", path, fd);
    return fd;
}

static void nvdrv_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    nvdrv_log("request", rq->cmd, rq);
    switch (rq->cmd) {
    case NVDRV_INITIALIZE: {
        /* Handle arguments are transferred as copy/move handles.  The data is
         * the transfer-memory size in some SDK versions and an opaque u64 in
         * others; the host has no GPU, so the important contract is a real
         * successful initialization when a valid session is present. */
        if (rq->data_size < 4 && !rq->ncopy && !rq->nmove) { rs->result = nvdrv_bad(rq->cmd); return; }
        if (trace_enabled())
            fprintf(stderr, "[totk-nvdrv] Initialize handles copy=%u move=%u data=%u\n",
                    rq->ncopy, rq->nmove, rq->data_size);
        IPC_OUT(rs, (uint32_t){0});
        return;
    }
    case NVDRV_SET_ARUID:
    case NVDRV_SET_ARUID_WITHOUT_CHECK: {
        uint64_t aruid = 0;
        if (rq->data_size >= 8) memcpy(&aruid, rq->data, 8);
        else if (rq->data_size >= 4) { uint32_t v; memcpy(&v, rq->data, 4); aruid = v; }
        if (trace_enabled()) fprintf(stderr, "[totk-nvdrv] SetAruid %#llx\n", (unsigned long long)aruid);
        IPC_OUT(rs, (uint32_t){0});
        return;
    }
    case NVDRV_SET_GRAPHICS_FIRMWARE_MEMORY_MARGIN: {
        uint64_t v = 0;
        if (rq->data_size >= 8) memcpy(&v, rq->data, 8);
        if (trace_enabled()) fprintf(stderr, "[totk-nvdrv] gfx margin %#llx\n", (unsigned long long)v);
        return;
    }
    case NVDRV_OPEN: {
        const char *path = "";
        if (rq->nx < 1 || !rq->x[0].addr || !rq->x[0].size) { rs->result = nvdrv_bad(rq->cmd); return; }
        path = ipc_guest(rq->x[0].addr, rq->x[0].size);
        char clean[64];
        size_t n = strnlen(path, sizeof clean - 1);
        memcpy(clean, path, n); clean[n] = 0;
        uint32_t fd = nvdrv_open_path(clean);
        uint32_t err = fd ? 0 : 0x1a8;
        IPC_OUT(rs, fd);
        IPC_OUT(rs, err);
        return;
    }
    case NVDRV_CLOSE: {
        uint32_t fd = rq->data_size >= 4 ? IPC_ARG(rq, uint32_t, 0) : 0;
        if (fd < NVDRV_MAX_FD && nvdrv_fds[fd].used) {
            if (trace_enabled()) fprintf(stderr, "[totk-nvdrv] close fd %#x (%s)\n", fd, nvdrv_fds[fd].path);
            memset(&nvdrv_fds[fd], 0, sizeof nvdrv_fds[fd]);
        }
        IPC_OUT(rs, (uint32_t){0});
        return;
    }
    case NVDRV_IOCTL:
    case NVDRV_IOCTL2:
    case NVDRV_IOCTL3: {
        if (rq->data_size < 8) { rs->result = nvdrv_bad(rq->cmd); return; }
        uint32_t fd, code;
        memcpy(&fd, rq->data, 4);
        memcpy(&code, rq->data + 4, 4);
        uint8_t *in = NULL, *out = NULL;
        uint32_t in_size = 0, out_size = 0;
        if (rq->na) { in = ipc_guest(rq->a[0].addr, rq->a[0].size); in_size = (uint32_t)rq->a[0].size; }
        if (rq->nb) { out = ipc_guest(rq->b[0].addr, rq->b[0].size); out_size = (uint32_t)rq->b[0].size; }
        if (!in && rq->nx) { in = ipc_guest(rq->x[0].addr, rq->x[0].size); in_size = (uint32_t)rq->x[0].size; }
        if (rq->nw) { out = ipc_guest(rq->w[0].addr, rq->w[0].size); out_size = (uint32_t)rq->w[0].size; }
        uint32_t err = nvdrv_ioctl(fd, code, in, in_size, out, out_size);
        if (trace_enabled())
            fprintf(stderr, "[totk-nvdrv] ioctl fd=%#x code=%#x in=%p/%u out=%p/%u err=%u\n",
                    fd, code, (void *)in, in_size, (void *)out, out_size, err);
        IPC_OUT(rs, err);
        return;
    }
    case NVDRV_QUERY_EVENT: {
        if (rq->data_size < 8) { rs->result = nvdrv_bad(rq->cmd); return; }
        uint32_t err = 0;
        IPC_OUT(rs, err);
        if (rs->nmove == sizeof rs->move / sizeof rs->move[0])
            runtime_fail("nvdrv too many move handles");
        rs->move[rs->nmove++] = runtime_event_new();
        return;
    }
    case NVDRV_MAP_SHARED_MEM: {
        /* TransferMemory handle + Fd + HMem.  The host nvmap object already
         * owns guest-visible memory, so the map is a real alias of that
         * object; unknown handle/transfer memory is an explicit error. */
        uint32_t fd = 0, hmem = 0;
        if (rq->data_size >= 8) { memcpy(&fd, rq->data, 4); memcpy(&hmem, rq->data + 4, 4); }
        uint32_t th = rq->ncopy ? rq->copy[0] : (rq->nmove ? rq->move[0] : 0);
        IpcTransferMemory *tm = th ? runtime_handle_get(th, 0) : NULL;
        nvdrv_fd *d = fd_get(fd);
        nvdrv_map *m = map_find(hmem);
        if (!d || !m || !tm || !tm->addr || m->size > tm->size) {
            fprintf(stderr, "[totk-nvdrv] ERROR MapSharedMem fd=%#x hmem=%#x tm=%#x\n", fd, hmem, th);
            rs->result = 0x1a8;
            return;
        }
        m->addr = tm->addr;
        IPC_OUT(rs, (uint32_t){0});
        return;
    }
    case NVDRV_GET_STATUS:
        /* The real status structure is version-specific; do not fabricate it. */
        fprintf(stderr, "[totk-nvdrv] ERROR GetStatus requested before a status layout is implemented\n");
        rs->result = 0x1a8;
        return;
    case NVDRV_DUMP_STATUS:
        return;
    case NVDRV_INITIALIZE_DEVTOOLS:
        fprintf(stderr, "[totk-nvdrv] ERROR InitializeDevtools not implemented\n");
        rs->result = 0x1a8;
        return;
    default:
        rs->result = nvdrv_bad(rq->cmd);
        return;
    }
}

/* Host-side protocol self-test used by the probe-nvdrv mode.  It exercises the
 * real CMIF handler with guest buffers and verifies the guest-address mapping. */
int runtime_nvdrv_selftest(void) {
    static const char path[] = "/dev/nvmap";
    IpcResponse rs;
    IpcRequest rq;
    uint32_t fd = 0, err = 0;
    memset(&rs, 0, sizeof rs);
    memset(&rq, 0, sizeof rq);
    rq.cmd = NVDRV_OPEN;
    rq.nx = 1;
    rq.x[0] = (IpcBuffer){(uint64_t)(uintptr_t)path, sizeof path, 0x5};
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 8) return 0;
    memcpy(&fd, rs.data, 4);
    memcpy(&err, rs.data + 4, 4);
    if (!fd || err) return 0;

    uint32_t create[2] = {0x20000, 0};
    uint32_t create_out[2] = {0, 0};
    uint32_t hdr[2] = {fd, NVMAP_IOC_CREATE};
    rq.cmd = NVDRV_IOCTL3;
    rq.data = (const uint8_t *)hdr;
    rq.data_size = 8;
    rq.na = 1; rq.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)create, sizeof create, 0};
    rq.nb = 1; rq.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)create_out, sizeof create_out, 0};
    memset(&rs, 0, sizeof rs);
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 4) return 0;
    uint32_t ioerr;
    memcpy(&ioerr, rs.data, 4);
    if (ioerr || !create_out[1]) return 0;
    uint32_t handle = create_out[1];

    uint32_t alloc[8] = {handle, 0, 1, 0x1000, 0, 0, 0, 0};
    uint8_t alloc_out[32] = {0};
    rq.data = (const uint8_t *)hdr;
    hdr[1] = NVMAP_IOC_ALLOC;
    rq.na = 1; rq.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)alloc, sizeof alloc, 0};
    rq.nb = 1; rq.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)alloc_out, sizeof alloc_out, 0};
    memset(&rs, 0, sizeof rs);
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 4) return 0;
    memcpy(&ioerr, rs.data, 4);
    if (ioerr) return 0;

    /* MMAP must hand back a guest address that is actually writable: the old
     * code could return an address whose guest-window pages had been
     * munmapped, which this touch catches. */
    uint8_t mmap_in[40] = {0}, mmap_out[40] = {0};
    memcpy(mmap_in, &handle, 4);
    hdr[1] = NVMAP_IOC_MMAP;
    rq.na = 1; rq.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)mmap_in, sizeof mmap_in, 0};
    rq.nb = 1; rq.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)mmap_out, sizeof mmap_out, 0};
    memset(&rs, 0, sizeof rs);
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 4) return 0;
    memcpy(&ioerr, rs.data, 4);
    uint64_t mapped = 0;
    memcpy(&mapped, mmap_out + 8, 8);
    if (ioerr || !mapped ||
        mapped < (uint64_t)(uintptr_t)nvdrv_guest_area ||
        mapped + 0x20000 > (uint64_t)(uintptr_t)nvdrv_guest_area + NVDRV_GUEST_SIZE) return 0;
    memset((void *)(uintptr_t)mapped, 0x5a, 0x20000);

    uint32_t param[3] = {handle, 1, 0};
    uint32_t param_out[3] = {0, 0, 0};
    uint8_t free_in[24] = {0}; memcpy(free_in, &handle, 4);
    uint8_t free_out[24] = {0};
    hdr[1] = NVMAP_IOC_PARAM;
    rq.na = 1; rq.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)param, sizeof param, 0};
    rq.nb = 1; rq.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)param_out, sizeof param_out, 0};
    memset(&rs, 0, sizeof rs);
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 4) return 0;
    memcpy(&ioerr, rs.data, 4);
    if (ioerr || param_out[2] != 0x20000) return 0;
    if (!nvdrv_guest_area || (uintptr_t)nvdrv_guest_area < 0x100000000ull ||
        (uintptr_t)nvdrv_guest_area >= (1ull << 39)) return 0;

    hdr[1] = NVMAP_IOC_FREE;
    rq.na = 1; rq.a[0] = (IpcBuffer){(uint64_t)(uintptr_t)free_in, sizeof free_in, 0};
    rq.nb = 1; rq.b[0] = (IpcBuffer){(uint64_t)(uintptr_t)free_out, sizeof free_out, 0};
    memset(&rs, 0, sizeof rs);
    nvdrv_handler(NULL, &rq, &rs);
    if (rs.data_size < 4) return 0;
    memcpy(&ioerr, rs.data, 4);
    /* FREE must zero the guest block; the window itself stays mapped. */
    if (ioerr || ((const uint8_t *)(uintptr_t)mapped)[0] != 0 ||
        ((const uint8_t *)(uintptr_t)mapped)[0x1ffff] != 0) return 0;

    uint32_t close_fd = fd;
    memset(&rs, 0, sizeof rs);
    memset(&rq, 0, sizeof rq);
    rq.cmd = NVDRV_CLOSE;
    rq.data = (const uint8_t *)&close_fd;
    rq.data_size = 4;
    nvdrv_handler(NULL, &rq, &rs);
    return 1;
}

__attribute__((constructor)) static void register_nvdrv(void) {
    runtime_ipc_register("nvdrv", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:a", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:s", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:t", nvdrv_handler, NULL, NULL);
}
