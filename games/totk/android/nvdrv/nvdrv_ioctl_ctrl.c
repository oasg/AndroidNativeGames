/* Module: ioctl_ctrl. */
#include "nvdrv_internal.h"

static uint32_t syncpt_value[64];

static int syncpt_used[64];

uint32_t host_ioctl_ctrl(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
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

uint32_t host_ioctl_generic(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size,
                                   uint8_t *out, uint32_t out_size) {
    (void)in; (void)in_size; (void)out; (void)out_size;
    fprintf(stderr, "[totk-nvdrv] ERROR ioctl %#x on %s not implemented\n",
            code, fd ? fd->path : "?");
    return 0x1a8;
}
