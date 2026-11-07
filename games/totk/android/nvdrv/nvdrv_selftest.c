/* Module: selftest. */
#include "nvdrv_internal.h"

/* Host-side protocol self-test used by the probe-nvdrv mode.  It exercises the
 * real CMIF handler with guest buffers and verifies the guest-address mapping. */
int runtime_nvdrv_selftest(void) {
    const char path[] = "/dev/nvmap";
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
