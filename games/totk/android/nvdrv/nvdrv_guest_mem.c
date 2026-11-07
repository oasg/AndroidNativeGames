/* Module: guest_mem. */
#include "nvdrv_internal.h"

static nvdrv_map nvdrv_maps[NVDRV_MAX_MAP];

static uint32_t nvdrv_next_handle = 1;

static uint32_t nvdrv_next_map_id = 1;

static size_t nvdrv_guest_used;

int nvdrv_guest_init(void) {
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

void *nvdrv_guest_alloc(size_t size, size_t align) {
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

nvdrv_fd *fd_get(uint32_t fd) {
    if (fd >= NVDRV_MAX_FD || !nvdrv_fds[fd].used) return NULL;
    return &nvdrv_fds[fd];
}

nvdrv_map *map_find(uint32_t handle) {
    for (unsigned i = 0; i < NVDRV_MAX_MAP; i++)
        if (nvdrv_maps[i].used && nvdrv_maps[i].handle == handle) return &nvdrv_maps[i];
    return NULL;
}

nvdrv_map *map_find_id(uint32_t id) {
    for (unsigned i = 0; i < NVDRV_MAX_MAP; i++)
        if (nvdrv_maps[i].used && nvdrv_maps[i].id == id) return &nvdrv_maps[i];
    return NULL;
}

nvdrv_map *map_new(uint64_t size, uint64_t addr, uint32_t flags, uint32_t align,
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

void map_free(nvdrv_map *m) {
    if (!m || !m->used) return;
    /* The guest window is a bump allocator: a block cannot be handed back to
     * the OS, but zero it so a later object never observes freed contents. */
    if (m->host && m->guest && m->size) memset(m->host, 0, (size_t)m->size);
    memset(m, 0, sizeof *m);
}

__attribute__((constructor)) static void register_nvdrv(void) {
    runtime_ipc_register("nvdrv", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:a", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:s", nvdrv_handler, NULL, NULL);
    runtime_ipc_register("nvdrv:t", nvdrv_handler, NULL, NULL);
}
