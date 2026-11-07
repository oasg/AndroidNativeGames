/* NVN module: memory_pool. */
#include "nvn_internal.h"

static samp_pool_map_t g_samp_pool_map[NVN_POOL_MAP_MAX];

static int32_t g_samp_buckets[NVN_POOL_BUCKETS];

static int32_t g_samp_free;

static unsigned g_samp_pool_map_count;

static unsigned g_samp_pool_full;

static uint32_t g_pool_clock = 1;

static uint32_t g_pool_epoch = 1;

static int g_pool_map_ready;

static pthread_mutex_t g_pool_map_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned g_pool_epoch_next;

void pool_epoch_set(const void *p) {
    if (!p) return;
    pthread_mutex_lock(&g_pool_epoch_lock);
    for (unsigned i = 0; i < g_pool_epoch_count; i++)
        if (g_pool_epochs[i].p == p) {
            g_pool_epochs[i].epoch = g_pool_epoch;
            pthread_mutex_unlock(&g_pool_epoch_lock);
            return;
        }
    unsigned i = g_pool_epoch_count < NVN_POOL_EPOCH_MAX
               ? g_pool_epoch_count++ : g_pool_epoch_next++ % NVN_POOL_EPOCH_MAX;
    g_pool_epochs[i].p = p;
    g_pool_epochs[i].epoch = g_pool_epoch;
    pthread_mutex_unlock(&g_pool_epoch_lock);
}

uint32_t pool_epoch_get(const void *p) {
    uint32_t e = 0;
    if (!p) return 0;
    pthread_mutex_lock(&g_pool_epoch_lock);
    for (unsigned i = 0; i < g_pool_epoch_count; i++)
        if (g_pool_epochs[i].p == p) { e = g_pool_epochs[i].epoch; break; }
    pthread_mutex_unlock(&g_pool_epoch_lock);
    return e;
}

/* ------------------------------------------------------------------ */
/* Memory pool                                                         */

void nvnMemoryPoolBuilderSetDefaults(pool_builder_t *b) {
    if (!b) return;
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
}

void nvnMemoryPoolBuilderSetDevice(pool_builder_t *b, void *d) { if (b) b->dev = d; }

void nvnMemoryPoolBuilderSetStorage(pool_builder_t *b, void *m, size_t n) {
    if (b) { b->mem = m; b->size = n; }
}

void nvnMemoryPoolBuilderSetFlags(pool_builder_t *b, int f) { if (b) b->flags = (uint32_t)f; }

void *nvnMemoryPoolBuilderGetMemory(const pool_builder_t *b) { return b ? b->mem : NULL; }

size_t nvnMemoryPoolBuilderGetSize(const pool_builder_t *b) { return b ? (size_t)b->size : 0; }

int nvnMemoryPoolBuilderGetFlags(const pool_builder_t *b) { return b ? (int)b->flags : 0; }

void *nvnMemoryPoolBuilderGetDevice(const pool_builder_t *b) { return b ? b->dev : NULL; }

uint32_t nvnMemoryPoolInitialize(pool_t *p, const pool_builder_t *b) { nvn_api("nvnMemoryPoolInitialize");
    if (!p || !b) { nvn_fail("nvnMemoryPoolInitialize", "null pool/builder"); return 0; }
    memset(p, 0, sizeof *p);
    p->magic = NVN_MAGIC('P', 'O', 'O', 'L');
    p->flags = b->flags;
    p->size = b->size;
    p->dev = b->dev;
    p->mem = b->mem;
    if (!p->mem && p->size) {
        /* Physical/virtual pools do not have guest storage in this host.
         * Give them a private CPU mapping so map/address calls have real
         * pointer semantics; no GPU sees this memory. */
        void *m = mmap(NULL, (size_t)p->size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) {
            char d[64]; snprintf(d, sizeof d, "size %#llx", (unsigned long long)p->size);
            nvn_fail("nvnMemoryPoolInitialize", d);
            p->magic = 0;
            return 0;
        }
        p->mem = m;
        p->owned = 1;
        p->host_size = p->size;
    }
    if (!p->mem && !(p->flags & NVN_POOL_GPU_NO_ACCESS)) {
        nvn_fail("nvnMemoryPoolInitialize", "pool has no storage");
        p->magic = 0;
        return 0;
    }
    if (nvn_trace_first() && nvn_map_trace++ < 2048)
        fprintf(stderr, "[totk-nvn] MemoryPoolInitialize %p mem=%p size=%#llx flags=%#x\n",
                (void *)p, (void *)p->mem, (unsigned long long)p->size, p->flags);
    if (getenv("TOTK_ANDROID_TRACE_NVN"))
        fprintf(stderr, "[totk-nvn] MemoryPoolInitialize %p mem=%p size=%#llx flags=%#x%s\n",
                (void *)p, (void *)p->mem, (unsigned long long)p->size, p->flags,
                p->owned ? " (host fallback)" : "");
    return 1;
}

void nvnMemoryPoolFinalize(pool_t *p) {
    if (!p) return;
    if (p->owned && p->mem) munmap(p->mem, (size_t)p->host_size);
    p->magic = 0;
}

void nvnMemoryPoolSetDebugLabel(pool_t *p, const char *l) { (void)p; (void)l; }

void *nvnMemoryPoolMap(const pool_t *p) { nvn_api("nvnMemoryPoolMap");
    if (!p || p->magic != NVN_MAGIC('P', 'O', 'O', 'L')) {
        nvn_fail("nvnMemoryPoolMap", "invalid pool object");
        return NULL;
    }
    /* A CPU reader must observe every GPU write submitted before this call. */
    if (!nvn_gpu_drain("nvnMemoryPoolMap")) return NULL;
    if (p->flags & NVN_POOL_CPU_NO_ACCESS) {
        /* NVN marks this pool CPU_NO_ACCESS for the guest, but the HLE keeps
         * all GPU resources in host memory and needs the mapping for shader,
         * vertex and index translation.  Return it; the guest cannot tell. */
        if (nvn_trace())
            fprintf(stderr, "[totk-nvn] MemoryPoolMap CPU_NO_ACCESS pool=%p -> host mapping %p\n",
                    (const void *)p, (void *)p->mem);
        return p->mem;
    }
    return p->mem;
}

void nvnMemoryPoolFlushMappedRange(const pool_t *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }

void nvnMemoryPoolInvalidateMappedRange(const pool_t *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }

uint64_t nvnMemoryPoolGetBufferAddress(const pool_t *p) { nvn_api("nvnMemoryPoolGetBufferAddress");
    return p && p->magic == NVN_MAGIC('P', 'O', 'O', 'L') ? (uint64_t)(uintptr_t)p->mem : 0;
}

uint32_t nvnMemoryPoolMapVirtual(pool_t *p, int n, const void *reqs) {
    (void)reqs;
    if (!nvn_obj(p, NVN_MAGIC('P', 'O', 'O', 'L'), "nvnMemoryPoolMapVirtual")) return 0;
    if (n <= 0) { nvn_fail("nvnMemoryPoolMapVirtual", "empty mapping request"); return 0; }
    p->map_count += (uint32_t)n;
    return 1;
}

size_t nvnMemoryPoolGetSize(const pool_t *p) { return p ? (size_t)p->size : 0; }

int nvnMemoryPoolGetFlags(const pool_t *p) { return p ? (int)p->flags : 0; }

unsigned pool_home(const void *p, int id) {
    uint64_t h = (uint64_t)(uintptr_t)p ^ ((uint64_t)(uint32_t)id << 32);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (unsigned)h & (NVN_POOL_BUCKETS - 1);
}

int pool_old(const tex_pool_t *p) {
    return (int32_t)(g_pool_epoch - pool_epoch_get(p)) > 2;
}

void pool_map_init_locked(void) {
    if (g_pool_map_ready) return;
    for (unsigned i = 0; i < NVN_POOL_BUCKETS; i++) {
        g_tex_buckets[i] = g_samp_buckets[i] = -1;
    }
    for (unsigned i = 0; i < NVN_POOL_MAP_MAX; i++) {
        g_tex_pool_map[i].next = i + 1 < NVN_POOL_MAP_MAX ? (int32_t)i + 1 : -1;
        g_samp_pool_map[i].next = i + 1 < NVN_POOL_MAP_MAX ? (int32_t)i + 1 : -1;
    }
    g_tex_free = g_samp_free = 0;
    g_pool_map_ready = 1;
}

void nvn_tex_pool_forget(const tex_pool_t *p) {
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    for (unsigned b = 0; b < NVN_POOL_BUCKETS; b++) {
        int32_t prev = -1, cur = g_tex_buckets[b];
        while (cur >= 0) {
            tex_pool_map_t *e = &g_tex_pool_map[cur];
            int32_t next = e->next;
            if (e->pool == p) {
                if (prev < 0) g_tex_buckets[b] = next;
                else g_tex_pool_map[prev].next = next;
                e->next = g_tex_free; g_tex_free = cur;
                if (g_tex_pool_map_count) g_tex_pool_map_count--;
            } else {
                prev = cur;
            }
            cur = next;
        }
    }
    pthread_mutex_unlock(&g_pool_map_lock);
}

void nvn_tex_pool_add(const tex_pool_t *p, int id, const tex_t *t) {
    if (!tex_pool_valid(p)) return;
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    uint32_t sig = tex_pool_sig(p);
    unsigned b = pool_home(p, id);
    for (int32_t cur = g_tex_buckets[b]; cur >= 0; cur = g_tex_pool_map[cur].next) {
        tex_pool_map_t *e = &g_tex_pool_map[cur];
        if (e->pool == p && e->id == (uint32_t)id) {
            e->tex = t; e->gen = p->gen; e->sig = sig;
            e->age = ++g_pool_clock; pool_epoch_set(p);
            pthread_mutex_unlock(&g_pool_map_lock);
            return;
        }
    }
    if (g_tex_free < 0) {
        tex_pool_prune_locked();
        if (g_tex_free < 0) {
            g_tex_pool_full++;
            static int warned;
            if (!warned++) fprintf(stderr, "[totk-nvn] ERROR texture pool map full (%u registrations); later binds resolve NULL\n",
                                   g_tex_pool_map_count);
            pthread_mutex_unlock(&g_pool_map_lock);
            return;
        }
    }
    int32_t idx = g_tex_free;
    g_tex_free = g_tex_pool_map[idx].next;
    tex_pool_map_t *e = &g_tex_pool_map[idx];
    e->pool = p; e->tex = t; e->id = (uint32_t)id; e->gen = p->gen; e->sig = sig;
    e->age = ++g_pool_clock;
    e->next = g_tex_buckets[b];
    g_tex_buckets[b] = idx;
    g_tex_pool_map_count++;
    if (g_tex_pool_map_count > g_tex_pool_max) g_tex_pool_max = g_tex_pool_map_count;
    /* Do not write host bookkeeping into the guest NVNtexturePool object:
     * the engine keeps its own links next to the NVN header, and an epoch
     * store here corrupted the texture list (main+0x79a9a4). */
    pthread_mutex_unlock(&g_pool_map_lock);
}

const tex_t *nvn_tex_pool_lookup(const tex_pool_t *p, int id) {
    if (!tex_pool_valid(p)) return NULL;
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    uint32_t sig = tex_pool_sig(p);
    unsigned b = pool_home(p, id);
    int32_t prev = -1, cur = g_tex_buckets[b];
    while (cur >= 0) {
        tex_pool_map_t *e = &g_tex_pool_map[cur];
        int32_t next = e->next;
        if (e->pool == p && e->id == (uint32_t)id) {
            if (e->gen != p->gen || e->sig != sig) {
                if (prev < 0) g_tex_buckets[b] = next;
                else g_tex_pool_map[prev].next = next;
                e->next = g_tex_free; g_tex_free = cur;
                if (g_tex_pool_map_count) g_tex_pool_map_count--;
                pthread_mutex_unlock(&g_pool_map_lock);
                return NULL;
            }
            e->age = ++g_pool_clock;
            const tex_t *t = e->tex;
            pthread_mutex_unlock(&g_pool_map_lock);
            return t;
        }
        prev = cur; cur = next;
    }
    pthread_mutex_unlock(&g_pool_map_lock);
    return NULL;
}

void nvn_samp_pool_forget(const samp_pool_t *p) {
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    int32_t prev = -1, cur = -1;
    (void)prev; (void)cur;
    unsigned n = 0;
    for (unsigned i = 0; i < g_samp_pool_map_count; i++)
        if (g_samp_pool_map[i].pool != p) g_samp_pool_map[n++] = g_samp_pool_map[i];
    g_samp_pool_map_count = n;
    pthread_mutex_unlock(&g_pool_map_lock);
}

void nvn_samp_pool_add(const samp_pool_t *p, int id, const samp_t *s) {
    if (!samp_pool_valid(p)) return;
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    for (unsigned i = 0; i < g_samp_pool_map_count; i++)
        if (g_samp_pool_map[i].pool == p && g_samp_pool_map[i].id == (uint32_t)id) {
            g_samp_pool_map[i].samp = s; g_samp_pool_map[i].gen = p->gen;
            g_samp_pool_map[i].age = ++g_pool_clock; pool_epoch_set(p);
            pthread_mutex_unlock(&g_pool_map_lock);
            return;
        }
    unsigned n = 0;
    for (unsigned i = 0; i < g_samp_pool_map_count; i++) {
        samp_pool_map_t *e = &g_samp_pool_map[i];
        if (!samp_pool_valid(e->pool) || e->gen != e->pool->gen ||
            (int32_t)(g_pool_epoch - pool_epoch_get(e->pool)) > 2) continue;
        if (n != i) g_samp_pool_map[n] = *e;
        n++;
    }
    g_samp_pool_map_count = n;
    if (g_samp_pool_map_count >= NVN_POOL_MAP_MAX) {
        g_samp_pool_full++;
        pthread_mutex_unlock(&g_pool_map_lock);
        return;
    }
    samp_pool_map_t *e = &g_samp_pool_map[g_samp_pool_map_count++];
    e->pool = p; e->samp = s; e->id = (uint32_t)id; e->gen = p->gen;
    e->age = ++g_pool_clock;
    pool_epoch_set(p);
    pthread_mutex_unlock(&g_pool_map_lock);
}

const samp_t *nvn_samp_pool_lookup(const samp_pool_t *p, int id) {
    if (!samp_pool_valid(p)) return NULL;
    pthread_mutex_lock(&g_pool_map_lock);
    pool_map_init_locked();
    for (unsigned i = 0; i < g_samp_pool_map_count; i++) {
        samp_pool_map_t *e = &g_samp_pool_map[i];
        if (e->pool == p && e->id == (uint32_t)id) {
            if (e->gen != p->gen) { pthread_mutex_unlock(&g_pool_map_lock); return NULL; }
            e->age = ++g_pool_clock; pool_epoch_set(p);
            const samp_t *s = e->samp;
            pthread_mutex_unlock(&g_pool_map_lock);
            return s;
        }
    }
    pthread_mutex_unlock(&g_pool_map_lock);
    return NULL;
}

void nvn_pool_epoch_tick(void) {
    uint32_t e = __atomic_add_fetch(&g_pool_epoch, 1, __ATOMIC_RELAXED);
    if (!e) __atomic_store_n(&g_pool_epoch, 1, __ATOMIC_RELAXED);
}

const tex_pool_t *nvn_tex_pool_for(const cb_t *c) {
    if (c && c->tex_pool) return c->tex_pool;
    pthread_mutex_lock(&g_pool_lock);
    const tex_pool_t *p = g_last_tex_pool;
    pthread_mutex_unlock(&g_pool_lock);
    return p;
}

const samp_pool_t *nvn_samp_pool_for(const cb_t *c) {
    if (c && c->samp_pool) return c->samp_pool;
    pthread_mutex_lock(&g_pool_lock);
    const samp_pool_t *p = g_last_samp_pool;
    pthread_mutex_unlock(&g_pool_lock);
    return p;
}

void nvn_pool_forget(const void *pool) {
    pthread_mutex_lock(&g_pool_lock);
    if ((const void *)g_last_tex_pool == pool) g_last_tex_pool = NULL;
    if ((const void *)g_last_samp_pool == pool) g_last_samp_pool = NULL;
    pthread_mutex_unlock(&g_pool_lock);
}
