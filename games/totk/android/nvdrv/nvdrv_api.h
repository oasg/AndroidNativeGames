/* Internal module API. */
#ifndef TOTK_ANDROID_NVDRV_API_0_H
#define TOTK_ANDROID_NVDRV_API_0_H
#include "nvdrv_state.h"

extern void runtime_guest_commit(uintptr_t base, uintptr_t size);

extern void runtime_guest_map_extra(uintptr_t base, uintptr_t size);

int nvdrv_guest_init(void);

void *nvdrv_guest_alloc(size_t size, size_t align);

nvdrv_fd *fd_get(uint32_t fd);

nvdrv_map *map_find(uint32_t handle);

nvdrv_map *map_find_id(uint32_t id);

nvdrv_map *map_new(uint64_t size, uint64_t addr, uint32_t flags, uint32_t align, uint8_t kind, int host_alloc);

void map_free(nvdrv_map *m);

__attribute__((constructor)) static void register_nvdrv(void);

int trace_enabled(void);

void nvdrv_log(const char *what, uint32_t cmd, const IpcRequest *rq);

uint32_t nvdrv_bad(uint32_t cmd);

uint32_t host_ioctl_nvmap(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

uint32_t nvdrv_ioctl(uint32_t fd, uint32_t code, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

uint32_t host_ioctl_ctrl(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

uint32_t host_ioctl_generic(nvdrv_fd *fd, uint32_t code, uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size);

uint32_t nvdrv_open_path(const char *path);

void nvdrv_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

int runtime_nvdrv_selftest(void);

#endif
