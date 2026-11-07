/* Module: compute_0. */
#include "gfx_internal.h"

int vk_compute_desc_type(GfxBindingKind kind, VkDescriptorType *out) {
    switch (kind) {
    case GFX_BIND_UBO: *out = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; return 1;
    case GFX_BIND_SSBO: *out = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; return 1;
    case GFX_BIND_SAMPLED_IMAGE: *out = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; return 1;
    case GFX_BIND_COMBINED_IMAGE_SAMPLER:
        *out = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; return 1;
    case GFX_BIND_STORAGE_IMAGE: *out = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; return 1;
    default:
        return 0;
    }
}

uint32_t vk_compute_binding_count(const GfxBindingSpec *s) {
    if (s->kind == GFX_BIND_UBO || s->kind == GFX_BIND_SSBO) return 1;
    return s->count ? s->count : GFX_REFLECT_RUNTIME_ARRAY;
}

VkDeviceSize vk_compute_align_up(VkDeviceSize v, VkDeviceSize a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

int vk_compute_buffer_create(VkDeviceSize size, VkBufferUsageFlags usage,
                                    VkBuffer *buf, VkDeviceMemory *mem,
                                    void **mapped) {
    if (!size) size = 1;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!vk_ok(vkCreateBuffer(g.dev, &bci, NULL, buf), "vkCreateBuffer(compute)"))
        return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, *buf, &req);
    uint32_t mt = 0;
    if (!vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &mt)) {
        vk_log("ERROR compute buffer: no host-visible coherent memory type");
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = mt,
    };
    if (!vk_ok(vkAllocateMemory(g.dev, &mai, NULL, mem), "vkAllocateMemory(compute)")) {
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    if (!vk_ok(vkBindBufferMemory(g.dev, *buf, *mem, 0), "vkBindBufferMemory(compute)")) {
        vkFreeMemory(g.dev, *mem, NULL);
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    if (!vk_ok(vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, mapped),
               "vkMapMemory(compute)")) {
        vkFreeMemory(g.dev, *mem, NULL);
        vkDestroyBuffer(g.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    memset(*mapped, 0, (size_t)size);
    return 1;
}

void vk_compute_binding_release(GfxComputeBinding *b) {
    if (!b) return;
    if (g.dev) {
        if (b->mapped && b->mem) vkUnmapMemory(g.dev, b->mem);
        if (b->buffer) vkDestroyBuffer(g.dev, b->buffer, NULL);
        if (b->mem) vkFreeMemory(g.dev, b->mem, NULL);
    }
    memset(b, 0, sizeof *b);
    b->kind = (GfxBindingKind)-1;
}

void *vk_compute_compile_thread(void *arg) {
    VkComputeCompileJob *job = arg;
    job->result = vkCreateComputePipelines(g.dev, job->cache, 1, job->ci, NULL, job->out);
    return NULL;
}

int vk_compute_find_spec(const GfxComputePipeline *p, uint32_t set, uint32_t binding) {
    if (!p || set != 0) return -1;
    for (uint32_t i = 0; i < p->nspec; i++)
        if (p->spec[i].binding == binding) return (int)i;
    return -1;
}

void vk_compute_pipeline_destroy_locked(GfxComputePipeline *p) {
    if (!p) return;
    if (g.dev) {
        for (uint32_t i = 0; i < GFX_COMPUTE_MAX_BINDINGS; i++)
            vk_compute_binding_release(&p->bind[i]);
        if (p->pipe) vkDestroyPipeline(g.dev, p->pipe, NULL);
        if (p->layout) vkDestroyPipelineLayout(g.dev, p->layout, NULL);
        if (p->pool) vkDestroyDescriptorPool(g.dev, p->pool, NULL);
        if (p->dsl) vkDestroyDescriptorSetLayout(g.dev, p->dsl, NULL);
        if (p->cs) vkDestroyShaderModule(g.dev, p->cs, NULL);
    }
    free(p);
}

GfxComputePipeline *gfx_backend_compute_pipeline_create(const uint8_t *cs, size_t cs_size,
                                                        const GfxBindingSpec *specs,
                                                        uint32_t nspec) {
    if (!cs || cs_size < 20) {
        vk_log("ERROR compute pipeline: invalid SPIR-V module");
        return NULL;
    }
    if (!specs && nspec) {
        vk_log("ERROR compute pipeline: %u bindings but no specs", nspec);
        return NULL;
    }
    if (nspec > GFX_COMPUTE_MAX_BINDINGS) {
        vk_log("ERROR compute pipeline: %u bindings > %u", nspec, GFX_COMPUTE_MAX_BINDINGS);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (!vk_init_locked()) { pthread_mutex_unlock(&g_lock); return NULL; }

    double t0 = vk_now_ms();
    GfxComputePipeline *p = calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_lock); return NULL; }
    for (uint32_t i = 0; i < GFX_COMPUTE_MAX_BINDINGS; i++)
        p->bind[i].kind = (GfxBindingKind)-1;
    p->nspec = nspec;

    VkDescriptorSetLayoutBinding layout_b[GFX_COMPUTE_MAX_BINDINGS];
    VkDescriptorPoolSize pool[8];
    uint32_t npool = 0;
    int bad = 0;
    memset(pool, 0, sizeof pool);
    for (uint32_t i = 0; i < nspec; i++) {
        const GfxBindingSpec *s = &specs[i];
        VkDescriptorType dt;
        if (s->set != 0) {
            vk_log("ERROR compute pipeline: binding %u uses set %u (only set 0)",
                   s->binding, s->set);
            bad = 1;
            break;
        }
        if (!vk_compute_desc_type(s->kind, &dt)) {
            vk_log("ERROR compute pipeline: binding %u has unsupported kind %d",
                   s->binding, (int)s->kind);
            bad = 1;
            break;
        }
        uint32_t count = vk_compute_binding_count(s);
        if (!count || count > GFX_COMPUTE_MAX_BINDINGS) {
            vk_log("ERROR compute pipeline: binding %u count=%u", s->binding, count);
            bad = 1;
            break;
        }
        p->spec[i] = *s;
        p->spec[i].set = 0;
        p->spec[i].count = count;
        layout_b[i] = (VkDescriptorSetLayoutBinding){
            .binding = s->binding,
            .descriptorType = dt,
            .descriptorCount = count,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
        int found = -1;
        for (uint32_t j = 0; j < npool; j++)
            if (pool[j].type == dt) { found = (int)j; break; }
        if (found < 0) {
            if (npool >= sizeof pool / sizeof pool[0]) { bad = 1; break; }
            found = (int)npool++;
            pool[found].type = dt;
        }
        pool[found].descriptorCount += count;
    }
    if (bad) goto fail;

    if (!vk_shader_module(cs, cs_size, "vkCreateShaderModule(compute)", &p->cs))
        goto fail;
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = nspec,
        .pBindings = nspec ? layout_b : NULL,
    };
    if (!vk_ok(vkCreateDescriptorSetLayout(g.dev, &dlci, NULL, &p->dsl),
               "vkCreateDescriptorSetLayout(compute)"))
        goto fail;
    if (npool == 0) {
        /* A shader with no descriptors still needs a non-empty pool. */
        pool[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool[0].descriptorCount = 1;
        npool = 1;
    }
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = npool,
        .pPoolSizes = npool ? pool : NULL,
    };
    if (!vk_ok(vkCreateDescriptorPool(g.dev, &dpci, NULL, &p->pool),
               "vkCreateDescriptorPool(compute)"))
        goto fail;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &p->dsl,
    };
    if (!vk_ok(vkAllocateDescriptorSets(g.dev, &dsai, &p->dset),
               "vkAllocateDescriptorSets(compute)"))
        goto fail;
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &p->dsl,
    };
    if (!vk_ok(vkCreatePipelineLayout(g.dev, &lci, NULL, &p->layout),
               "vkCreatePipelineLayout(compute)"))
        goto fail;
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = p->cs,
            .pName = "main",
        },
        .layout = p->layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    if (!vk_ok(vk_create_compute_pipeline_bigstack(VK_NULL_HANDLE, &ci, &p->pipe),
               "vkCreateComputePipelines"))
        goto fail;
    vk_log("compute pipeline ready bindings=%u time=%.2f ms",
           nspec, vk_now_ms() - t0);
    pthread_mutex_unlock(&g_lock);
    return p;

fail:
    vk_compute_pipeline_destroy_locked(p);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void gfx_backend_compute_pipeline_destroy(GfxComputePipeline *pipeline) {
    if (!pipeline) return;
    pthread_mutex_lock(&g_lock);
    vk_compute_pipeline_destroy_locked(pipeline);
    pthread_mutex_unlock(&g_lock);
}

int gfx_backend_compute_bind_buffer(GfxComputePipeline *pipeline, uint32_t set,
                                    uint32_t binding, const void *data, size_t size) {
    if (!pipeline || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return 0; }
    int i = vk_compute_find_spec(pipeline, set, binding);
    if (i < 0) {
        vk_log("ERROR compute bind buffer: set=%u binding=%u not in pipeline", set, binding);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    GfxBindingSpec *s = &pipeline->spec[i];
    VkDescriptorType dt;
    if (!vk_compute_desc_type(s->kind, &dt) ||
        (s->kind != GFX_BIND_UBO && s->kind != GFX_BIND_SSBO)) {
        vk_log("ERROR compute bind buffer: binding %u is kind %d, not UBO/SSBO",
               binding, (int)s->kind);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkDeviceSize align = s->kind == GFX_BIND_UBO
        ? g.props.limits.minUniformBufferOffsetAlignment
        : g.props.limits.minStorageBufferOffsetAlignment;
    if (align < 4) align = 4;
    VkDeviceSize cap = vk_compute_align_up((VkDeviceSize)size, align);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkBufferUsageFlags usage = s->kind == GFX_BIND_UBO
        ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
        : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!vk_compute_buffer_create(cap, usage, &buf, &mem, &mapped)) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(mapped, data, size);

    GfxComputeBinding *b = &pipeline->bind[i];
    VkBuffer old_buf = b->buffer;
    VkDeviceMemory old_mem = b->mem;
    void *old_mapped = b->mapped;
    b->bound = 1;
    b->kind = s->kind;
    b->buffer = buf;
    b->mem = mem;
    b->mapped = mapped;
    b->capacity = cap;
    b->texture = NULL;
    b->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkDescriptorBufferInfo bi = { .buffer = buf, .offset = 0, .range = cap };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pipeline->dset,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = dt,
        .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    if (old_mapped && old_mem) vkUnmapMemory(g.dev, old_mem);
    if (old_buf) vkDestroyBuffer(g.dev, old_buf, NULL);
    if (old_mem) vkFreeMemory(g.dev, old_mem, NULL);
    if (getenv("TOTK_ANDROID_TRACE_COMPUTE"))
        vk_log("compute bind buffer set=%u binding=%u kind=%d size=%zu cap=%zu",
               set, binding, (int)s->kind, size, (size_t)cap);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_compute_bind_texture(GfxComputePipeline *pipeline, uint32_t set,
                                     uint32_t binding, const GfxTexture *texture) {
    if (!pipeline || !texture) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return 0; }
    int i = vk_compute_find_spec(pipeline, set, binding);
    if (i < 0) {
        vk_log("ERROR compute bind texture: set=%u binding=%u not in pipeline", set, binding);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    GfxBindingSpec *s = &pipeline->spec[i];
    VkDescriptorType dt;
    if (!vk_compute_desc_type(s->kind, &dt) ||
        (s->kind != GFX_BIND_SAMPLED_IMAGE &&
         s->kind != GFX_BIND_COMBINED_IMAGE_SAMPLER &&
         s->kind != GFX_BIND_STORAGE_IMAGE)) {
        vk_log("ERROR compute bind texture: binding %u is kind %d, not an image",
               binding, (int)s->kind);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    int storage = s->kind == GFX_BIND_STORAGE_IMAGE;
    if (storage && !texture->storage_ok) {
        vk_log("ERROR compute bind texture: %s texture %p was not created with "
               "storage usage; refusing to bind it as a storage image",
               vk_format_name(texture->format), (const void *)texture);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (!texture->view) {
        vk_log("ERROR compute bind texture: texture %p has no view",
               (const void *)texture);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    VkImageLayout want = storage ? VK_IMAGE_LAYOUT_GENERAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    uint32_t count = s->count;
    VkDescriptorImageInfo ii[GFX_COMPUTE_MAX_BINDINGS];
    for (uint32_t k = 0; k < count; k++) {
        ii[k] = (VkDescriptorImageInfo){
            .sampler = s->kind == GFX_BIND_COMBINED_IMAGE_SAMPLER
                           ? texture->sampler : VK_NULL_HANDLE,
            .imageView = texture->view,
            .imageLayout = want,
        };
    }
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pipeline->dset,
        .dstBinding = binding,
        .descriptorCount = count,
        .descriptorType = dt,
        .pImageInfo = ii,
    };
    vkUpdateDescriptorSets(g.dev, 1, &w, 0, NULL);
    GfxComputeBinding *b = &pipeline->bind[i];
    vk_compute_binding_release(b);
    b->bound = 1;
    b->kind = s->kind;
    b->texture = texture;
    b->layout = want;
    if (getenv("TOTK_ANDROID_TRACE_COMPUTE"))
        vk_log("compute bind texture set=%u binding=%u kind=%d texture=%p format=%s",
               set, binding, (int)s->kind, (const void *)texture,
               vk_format_name(texture->format));
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_compute_write_buffer(GfxComputePipeline *pipeline, uint32_t set,
                                     uint32_t binding, uint32_t offset,
                                     const void *data, size_t size) {
    if (!pipeline || !data || !size) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return 0; }
    int i = vk_compute_find_spec(pipeline, set, binding);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return 0; }
    GfxComputeBinding *b = &pipeline->bind[i];
    if (!b->bound || !b->mapped ||
        (b->kind != GFX_BIND_UBO && b->kind != GFX_BIND_SSBO) ||
        (uint64_t)offset + size > b->capacity) {
        vk_log("ERROR compute write buffer: set=%u binding=%u offset=%u size=%zu "
               "not bound or out of range", set, binding, offset, size);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy((uint8_t *)b->mapped + offset, data, size);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int gfx_backend_compute_read_buffer(GfxComputePipeline *pipeline, uint32_t set,
                                    uint32_t binding, void *dst, size_t size) {
    if (!pipeline || !dst || !size) return 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return 0; }
    int i = vk_compute_find_spec(pipeline, set, binding);
    if (i < 0) { pthread_mutex_unlock(&g_lock); return 0; }
    GfxComputeBinding *b = &pipeline->bind[i];
    if (!b->bound || !b->mapped ||
        (b->kind != GFX_BIND_UBO && b->kind != GFX_BIND_SSBO) ||
        size > b->capacity) {
        vk_log("ERROR compute read buffer: set=%u binding=%u size=%zu not bound "
               "or out of range", set, binding, size);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memcpy(dst, b->mapped, size);
    pthread_mutex_unlock(&g_lock);
    return 1;
}
