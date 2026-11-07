/* NVN module: buffer. */
#include "nvn_internal.h"

/* ------------------------------------------------------------------ */
/* Buffer                                                              */

void nvnBufferBuilderSetDefaults(buf_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
}

void nvnBufferBuilderSetDevice(buf_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnBufferBuilderSetStorage(buf_builder_t *b, void *p, ptrdiff_t off, size_t size) {
    if (b) { b->pool = p; b->off = off; b->size = size; }
}

void *nvnBufferBuilderGetDevice(const buf_builder_t *b) { return b ? b->dev : NULL; }

void *nvnBufferBuilderGetMemoryPool(const buf_builder_t *b) { return b ? b->pool : NULL; }

ptrdiff_t nvnBufferBuilderGetMemoryOffset(const buf_builder_t *b) { return b ? (ptrdiff_t)b->off : 0; }

size_t nvnBufferBuilderGetSize(const buf_builder_t *b) { return b ? (size_t)b->size : 0; }

uint32_t nvnBufferInitialize(buf_t *b, const buf_builder_t *s) { nvn_api("nvnBufferInitialize");
    if (!b || !s) { nvn_fail("nvnBufferInitialize", "null buffer/builder"); return 0; }
    pool_t *p = s->pool;
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnBufferInitialize(pool)")) return 0;
    if (s->off < 0 || (uint64_t)s->off > p->size || s->size > p->size - (uint64_t)s->off) {
        if (nvn_fail_count++ < 48)
            fprintf(stderr, "[totk-nvn] nvnBufferInitialize range off=%lld size=%llu pool_size=%llu\n",
                    (long long)s->off, (unsigned long long)s->size, (unsigned long long)p->size);
        char d[96]; snprintf(d, sizeof d, "range off=%lld size=%llu pool=%llu",
                             (long long)s->off, (unsigned long long)s->size,
                             (unsigned long long)p->size);
        nvn_fail("nvnBufferInitialize", d);
        return 0;
    }
    memset(b, 0, sizeof *b);
    b->magic = NVN_MAGIC('B', 'U', 'F', 0);
    b->pool = p;
    b->off = s->off;
    b->size = s->size;
    return 1;
}

void nvnBufferFinalize(buf_t *b) { if (b) b->magic = 0; }

void nvnBufferSetDebugLabel(buf_t *b, const char *l) { (void)b; (void)l; }

void *nvnBufferMap(const buf_t *b) { nvn_api("nvnBufferMap");
    nvn_buf_maps++;
    if (!b || b->magic != NVN_MAGIC('B', 'U', 'F', 0) || !b->pool || !b->pool->mem) {
        if (nvn_fail_count++ < 48)
            fprintf(stderr, "[totk-nvn] nvnBufferMap invalid buffer=%p magic=%#x pool=%p mem=%p off=%lld size=%llu\n",
                    (const void *)b, b ? b->magic : 0, b ? (void *)b->pool : NULL,
                    (b && b->pool) ? (void *)b->pool->mem : NULL,
                    (long long)(b ? b->off : 0), (unsigned long long)(b ? b->size : 0));
        return NULL;
    }
    if (!nvn_gpu_drain("nvnBufferMap")) return NULL;
    if (nvn_trace_first() && nvn_map_trace++ < 2048)
        fprintf(stderr, "[totk-nvn] BufferMap cb=%p buf=%p -> %p size=%llu\n",
                NULL, (const void *)b, (void *)(b->pool->mem + b->off),
                (unsigned long long)b->size);
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] BufferMap %p pool=%p mem=%p off=%lld size=%llu -> %p thread=%#x\n",
                (const void *)b, (void *)b->pool, (void *)b->pool->mem,
                (long long)b->off, (unsigned long long)b->size,
                (void *)(b->pool->mem + b->off), runtime_thread_current_handle());
    return b->pool->mem + b->off;
}

uint64_t nvnBufferGetAddress(const buf_t *b) {
    return (uint64_t)(uintptr_t)nvnBufferMap(b);
}

void nvnBufferFlushMappedRange(const buf_t *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }

void nvnBufferInvalidateMappedRange(const buf_t *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }

void *nvnBufferGetMemoryPool(const buf_t *b) { return b ? b->pool : NULL; }

ptrdiff_t nvnBufferGetMemoryOffset(const buf_t *b) { return b ? (ptrdiff_t)b->off : 0; }

size_t nvnBufferGetSize(const buf_t *b) { return b ? (size_t)b->size : 0; }

uint64_t nvnBufferGetDebugID(const buf_t *b) { return b ? (uint64_t)(uintptr_t)b : 0; }
