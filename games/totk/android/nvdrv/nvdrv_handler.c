/* Module: handler. */
#include "nvdrv_internal.h"

static uint32_t nvdrv_next_fd = 0x10;

uint32_t nvdrv_open_path(const char *path) {
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

void nvdrv_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
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
