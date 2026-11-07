/* Module: ioctl_nvmap. */
#include "nvdrv_internal.h"

static int nvdrv_trace = -1;

int trace_enabled(void) {
    int v = __atomic_load_n(&nvdrv_trace, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("TOTK_ANDROID_TRACE_NVDRV");
        v = e && *e && strcmp(e, "0") ? 1 : 0;
        __atomic_store_n(&nvdrv_trace, v, __ATOMIC_RELAXED);
    }
    return v;
}

void nvdrv_log(const char *what, uint32_t cmd, const IpcRequest *rq) {
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

uint32_t nvdrv_bad(uint32_t cmd) {
    fprintf(stderr, "[totk-nvdrv] ERROR unsupported nvdrv command %u\n", cmd);
    return 0x1a8; /* INVALID_PARAMETER-ish; callers surface a real failure */
}

uint32_t host_ioctl_nvmap(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
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

uint32_t nvdrv_ioctl(uint32_t fd, uint32_t code, uint8_t *in, uint32_t in_size,
                            uint8_t *out, uint32_t out_size) {
    nvdrv_fd *d = fd_get(fd);
    if (!d) return 0x1a8;
    if (d->is_nvmap) return host_ioctl_nvmap(d, code, in, in_size, out, out_size);
    if (d->is_ctrl) return host_ioctl_ctrl(d, code, in, in_size, out, out_size);
    return host_ioctl_generic(d, code, in, in_size, out, out_size);
}
