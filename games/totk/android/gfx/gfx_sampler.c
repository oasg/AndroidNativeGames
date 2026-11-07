/* Module: sampler. */
#include "gfx_internal.h"

int vk_make_sampler(VkSampler *sampler) {
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    return vk_ok(vkCreateSampler(g.dev, &sci, NULL, sampler),
                 "vkCreateSampler(reflection)");
}

VkFilter vk_nvn_filter(uint32_t f) {
    return f == 1 || f == 3 || f == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode vk_nvn_mipmap(uint32_t f) {
    return f >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode vk_nvn_wrap(uint32_t w) {
    switch (w) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkCompareOp vk_nvn_compare(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 8:
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

GfxSampler *gfx_backend_sampler_create(const GfxSamplerDesc *d) {
    if (!d) return NULL;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) { pthread_mutex_unlock(&g_lock); return NULL; }
    GfxSampler *s = calloc(1, sizeof *s);
    if (!s) { pthread_mutex_unlock(&g_lock); return NULL; }
    s->desc = *d;
    float max_lod = d->lod_max > 0.0f ? d->lod_max : VK_LOD_CLAMP_NONE;
    float min_lod = d->lod_min > 0.0f ? d->lod_min : 0.0f;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_nvn_filter(d->mag_filter),
        .minFilter = vk_nvn_filter(d->min_filter),
        .mipmapMode = vk_nvn_mipmap(d->min_filter),
        .addressModeU = vk_nvn_wrap(d->wrap_s),
        .addressModeV = vk_nvn_wrap(d->wrap_t),
        .addressModeW = vk_nvn_wrap(d->wrap_r),
        .mipLodBias = d->lod_bias,
        .minLod = min_lod,
        .maxLod = max_lod,
        .compareEnable = d->compare_enable ? VK_TRUE : VK_FALSE,
        .compareOp = vk_nvn_compare(d->compare_op),
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (!vk_ok(vkCreateSampler(g.dev, &sci, NULL, &s->sampler), "vkCreateSampler(nvn sampler)")) {
        free(s);
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    pthread_mutex_unlock(&g_lock);
    return s;
}

void gfx_backend_sampler_destroy(GfxSampler *s) {
    if (!s) return;
    pthread_mutex_lock(&g_lock);
    if (g.dev && s->sampler) vkDestroySampler(g.dev, s->sampler, NULL);
    pthread_mutex_unlock(&g_lock);
    free(s);
}

VkBlendFactor vk_nvn_blend_factor(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp vk_nvn_blend_op(uint32_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    case 1:
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp vk_nvn_depth_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 8: return VK_COMPARE_OP_ALWAYS;
    case 2:
    default: return VK_COMPARE_OP_LESS;
    }
}

VkStencilOp vk_nvn_stencil_op(uint32_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    case 1:
    default: return VK_STENCIL_OP_KEEP;
    }
}
