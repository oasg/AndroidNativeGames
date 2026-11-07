/* Type and object layouts. */
#ifndef TOTK_ANDROID_NVDRV_TYPES_0_H
#define TOTK_ANDROID_NVDRV_TYPES_0_H
#include "nvdrv_prelude.h"

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

#ifndef MAP_NORESERVE
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

#endif
