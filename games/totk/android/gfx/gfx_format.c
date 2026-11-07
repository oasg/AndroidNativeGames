/* Module: format. */
#include "gfx_internal.h"

static VkRtDimHint g_rt_dim_hints[8];

static unsigned g_rt_dim_hint_next;

VkFormatInfo vk_format_info(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return (VkFormatInfo){f, "R8_UNORM", 1};
    case VK_FORMAT_R8_SNORM: return (VkFormatInfo){f, "R8_SNORM", 1};
    case VK_FORMAT_R8_UINT: return (VkFormatInfo){f, "R8_UINT", 1};
    case VK_FORMAT_R8_SINT: return (VkFormatInfo){f, "R8_SINT", 1};
    case VK_FORMAT_R8G8_UNORM: return (VkFormatInfo){f, "R8G8_UNORM", 2};
    case VK_FORMAT_R8G8_SNORM: return (VkFormatInfo){f, "R8G8_SNORM", 2};
    case VK_FORMAT_R8G8_UINT: return (VkFormatInfo){f, "R8G8_UINT", 2};
    case VK_FORMAT_R8G8_SINT: return (VkFormatInfo){f, "R8G8_SINT", 2};
    case VK_FORMAT_R8G8B8_UNORM: return (VkFormatInfo){f, "R8G8B8_UNORM", 3};
    case VK_FORMAT_R8G8B8_SNORM: return (VkFormatInfo){f, "R8G8B8_SNORM", 3};
    case VK_FORMAT_R8G8B8_UINT: return (VkFormatInfo){f, "R8G8B8_UINT", 3};
    case VK_FORMAT_R8G8B8_SINT: return (VkFormatInfo){f, "R8G8B8_SINT", 3};
    case VK_FORMAT_R8G8B8A8_UNORM: return (VkFormatInfo){f, "R8G8B8A8_UNORM", 4};
    case VK_FORMAT_R8G8B8A8_SRGB: return (VkFormatInfo){f, "R8G8B8A8_SRGB", 4};
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return (VkFormatInfo){f, "R5G6B5_UNORM", 2};
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return (VkFormatInfo){f, "R4G4B4A4_UNORM", 2};
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return (VkFormatInfo){f, "R5G5B5A1_UNORM", 2};
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return (VkFormatInfo){f, "A2B10G10R10_UNORM", 4};
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return (VkFormatInfo){f, "A2B10G10R10_UINT", 4};
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return (VkFormatInfo){f, "B10G11R11_UFLOAT", 4};
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return (VkFormatInfo){f, "E5B9G9R9_UFLOAT", 4};
    case VK_FORMAT_R16_UNORM: return (VkFormatInfo){f, "R16_UNORM", 2};
    case VK_FORMAT_R16_SNORM: return (VkFormatInfo){f, "R16_SNORM", 2};
    case VK_FORMAT_R16_UINT: return (VkFormatInfo){f, "R16_UINT", 2};
    case VK_FORMAT_R16_SINT: return (VkFormatInfo){f, "R16_SINT", 2};
    case VK_FORMAT_R16_SFLOAT: return (VkFormatInfo){f, "R16_SFLOAT", 2};
    case VK_FORMAT_R16G16_UNORM: return (VkFormatInfo){f, "R16G16_UNORM", 4};
    case VK_FORMAT_R16G16_SNORM: return (VkFormatInfo){f, "R16G16_SNORM", 4};
    case VK_FORMAT_R16G16_UINT: return (VkFormatInfo){f, "R16G16_UINT", 4};
    case VK_FORMAT_R16G16_SINT: return (VkFormatInfo){f, "R16G16_SINT", 4};
    case VK_FORMAT_R16G16_SFLOAT: return (VkFormatInfo){f, "R16G16_SFLOAT", 4};
    case VK_FORMAT_R16G16B16_UNORM: return (VkFormatInfo){f, "R16G16B16_UNORM", 6};
    case VK_FORMAT_R16G16B16_SNORM: return (VkFormatInfo){f, "R16G16B16_SNORM", 6};
    case VK_FORMAT_R16G16B16_UINT: return (VkFormatInfo){f, "R16G16B16_UINT", 6};
    case VK_FORMAT_R16G16B16_SINT: return (VkFormatInfo){f, "R16G16B16_SINT", 6};
    case VK_FORMAT_R16G16B16_SFLOAT: return (VkFormatInfo){f, "R16G16B16_SFLOAT", 6};
    case VK_FORMAT_R16G16B16A16_UNORM: return (VkFormatInfo){f, "R16G16B16A16_UNORM", 8};
    case VK_FORMAT_R16G16B16A16_SNORM: return (VkFormatInfo){f, "R16G16B16A16_SNORM", 8};
    case VK_FORMAT_R16G16B16A16_UINT: return (VkFormatInfo){f, "R16G16B16A16_UINT", 8};
    case VK_FORMAT_R16G16B16A16_SINT: return (VkFormatInfo){f, "R16G16B16A16_SINT", 8};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return (VkFormatInfo){f, "R16G16B16A16_SFLOAT", 8};
    case VK_FORMAT_R32_SFLOAT: return (VkFormatInfo){f, "R32_SFLOAT", 4};
    case VK_FORMAT_R32_UINT: return (VkFormatInfo){f, "R32_UINT", 4};
    case VK_FORMAT_R32_SINT: return (VkFormatInfo){f, "R32_SINT", 4};
    case VK_FORMAT_R32G32_SFLOAT: return (VkFormatInfo){f, "R32G32_SFLOAT", 8};
    case VK_FORMAT_R32G32_UINT: return (VkFormatInfo){f, "R32G32_UINT", 8};
    case VK_FORMAT_R32G32_SINT: return (VkFormatInfo){f, "R32G32_SINT", 8};
    case VK_FORMAT_R32G32B32_SFLOAT: return (VkFormatInfo){f, "R32G32B32_SFLOAT", 12};
    case VK_FORMAT_R32G32B32_UINT: return (VkFormatInfo){f, "R32G32B32_UINT", 12};
    case VK_FORMAT_R32G32B32_SINT: return (VkFormatInfo){f, "R32G32B32_SINT", 12};
    case VK_FORMAT_R32G32B32A32_SFLOAT: return (VkFormatInfo){f, "R32G32B32A32_SFLOAT", 16};
    case VK_FORMAT_R32G32B32A32_UINT: return (VkFormatInfo){f, "R32G32B32A32_UINT", 16};
    case VK_FORMAT_R32G32B32A32_SINT: return (VkFormatInfo){f, "R32G32B32A32_SINT", 16};
    case VK_FORMAT_B8G8R8A8_UNORM: return (VkFormatInfo){f, "B8G8R8A8_UNORM", 4};
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return (VkFormatInfo){f, "A2B10G10R10_SNORM", 4};
    case VK_FORMAT_D16_UNORM: return (VkFormatInfo){f, "D16_UNORM", 2};
    case VK_FORMAT_X8_D24_UNORM_PACK32: return (VkFormatInfo){f, "X8_D24_UNORM", 4};
    case VK_FORMAT_D24_UNORM_S8_UINT: return (VkFormatInfo){f, "D24_UNORM_S8", 4};
    case VK_FORMAT_D32_SFLOAT: return (VkFormatInfo){f, "D32_SFLOAT", 4};
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return (VkFormatInfo){f, "D32_SFLOAT_S8", 8};
    case VK_FORMAT_S8_UINT: return (VkFormatInfo){f, "S8_UINT", 1};
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return (VkFormatInfo){f, "BC1_RGB_UNORM", 8};
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return (VkFormatInfo){f, "BC1_RGBA_UNORM", 8};
    case VK_FORMAT_BC2_UNORM_BLOCK: return (VkFormatInfo){f, "BC2_UNORM", 16};
    case VK_FORMAT_BC3_UNORM_BLOCK: return (VkFormatInfo){f, "BC3_UNORM", 16};
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return (VkFormatInfo){f, "BC1_RGB_SRGB", 8};
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return (VkFormatInfo){f, "BC1_RGBA_SRGB", 8};
    case VK_FORMAT_BC2_SRGB_BLOCK: return (VkFormatInfo){f, "BC2_SRGB", 16};
    case VK_FORMAT_BC3_SRGB_BLOCK: return (VkFormatInfo){f, "BC3_SRGB", 16};
    case VK_FORMAT_BC4_UNORM_BLOCK: return (VkFormatInfo){f, "BC4_UNORM", 8};
    case VK_FORMAT_BC4_SNORM_BLOCK: return (VkFormatInfo){f, "BC4_SNORM", 8};
    case VK_FORMAT_BC5_UNORM_BLOCK: return (VkFormatInfo){f, "BC5_UNORM", 16};
    case VK_FORMAT_BC5_SNORM_BLOCK: return (VkFormatInfo){f, "BC5_SNORM", 16};
    case VK_FORMAT_BC7_UNORM_BLOCK: return (VkFormatInfo){f, "BC7_UNORM", 16};
    case VK_FORMAT_BC7_SRGB_BLOCK: return (VkFormatInfo){f, "BC7_SRGB", 16};
    default: return (VkFormatInfo){VK_FORMAT_UNDEFINED, "UNSUPPORTED", 0};
    }
}

int gfx_backend_vk_format_for_nvn(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: return VK_FORMAT_X8_D24_UNORM_PACK32;
    case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 0x3A: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    /* NVN "unconverted" vertex formats. */
    case 0x51: return VK_FORMAT_R8_UINT;
    case 0x52: return VK_FORMAT_R8_SINT;
    case 0x53: return VK_FORMAT_R16_UINT;
    case 0x54: return VK_FORMAT_R16_SINT;
    case 0x55: return VK_FORMAT_R32_UINT;
    case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_UINT;
    case 0x58: return VK_FORMAT_R8G8_SINT;
    case 0x59: return VK_FORMAT_R16G16_UINT;
    case 0x5A: return VK_FORMAT_R16G16_SINT;
    case 0x5B: return VK_FORMAT_R32G32_UINT;
    case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_UINT;
    case 0x5E: return VK_FORMAT_R8G8B8_SINT;
    case 0x5F: return VK_FORMAT_R16G16B16_UINT;
    case 0x60: return VK_FORMAT_R16G16B16_SINT;
    case 0x61: return VK_FORMAT_R32G32B32_UINT;
    case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x64: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x65: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x66: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
    /* NVN_FORMAT_BGRX8/BGRA8, also usable as vertex formats. */
    case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* Ryujinx.Graphics.Shader.TextureFormat, in declaration order.  The .res.json
 * writes the enum name, the Android loader converts it back to this number. */
int gfx_backend_vk_format_for_texture_format(uint32_t f) {
    switch (f) {
    case 1:  return VK_FORMAT_R8_UNORM;
    case 2:  return VK_FORMAT_R8_SNORM;
    case 3:  return VK_FORMAT_R8_UINT;
    case 4:  return VK_FORMAT_R8_SINT;
    case 5:  return VK_FORMAT_R16_SFLOAT;
    case 6:  return VK_FORMAT_R16_UNORM;
    case 7:  return VK_FORMAT_R16_SNORM;
    case 8:  return VK_FORMAT_R16_UINT;
    case 9:  return VK_FORMAT_R16_SINT;
    case 10: return VK_FORMAT_R32_SFLOAT;
    case 11: return VK_FORMAT_R32_UINT;
    case 12: return VK_FORMAT_R32_SINT;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R8G8_SNORM;
    case 15: return VK_FORMAT_R8G8_UINT;
    case 16: return VK_FORMAT_R8G8_SINT;
    case 17: return VK_FORMAT_R16G16_SFLOAT;
    case 18: return VK_FORMAT_R16G16_UNORM;
    case 19: return VK_FORMAT_R16G16_SNORM;
    case 20: return VK_FORMAT_R16G16_UINT;
    case 21: return VK_FORMAT_R16G16_SINT;
    case 22: return VK_FORMAT_R32G32_SFLOAT;
    case 23: return VK_FORMAT_R32G32_UINT;
    case 24: return VK_FORMAT_R32G32_SINT;
    case 25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 27: return VK_FORMAT_R8G8B8A8_UINT;
    case 28: return VK_FORMAT_R8G8B8A8_SINT;
    case 29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 30: return VK_FORMAT_R16G16B16A16_UNORM;
    case 31: return VK_FORMAT_R16G16B16A16_SNORM;
    case 32: return VK_FORMAT_R16G16B16A16_UINT;
    case 33: return VK_FORMAT_R16G16B16A16_SINT;
    case 34: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 35: return VK_FORMAT_R32G32B32A32_UINT;
    case 36: return VK_FORMAT_R32G32B32A32_SINT;
    case 37: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 38: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 39: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int gfx_backend_vk_format_bpp(int vk_format) {
    VkFormatInfo i = vk_format_info((VkFormat)vk_format);
    return (int)i.bpp;
}

const char *vk_format_name(VkFormat f) {
    return vk_format_info(f).name;
}

int vk_format_is_color_target(VkFormat f) {
    switch (f) {
    case VK_FORMAT_UNDEFINED:
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 0;
    default:
        return vk_format_info(f).bpp != 0;
    }
}

int vk_format_is_depth(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

int vk_format_is_compressed(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 1;
    default:
        return 0;
    }
}

/* Tightly packed block geometry for one formatted image slice.  Compressed
 * formats are 4x4 blocks sized 8 or 16 bytes; everything else is 1x1 texels. */
int vk_format_block(VkFormat f, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    VkFormatInfo info = vk_format_info(f);
    if (info.bpp == 0) return 0;
    if (vk_format_is_compressed(f)) {
        *bw = 4; *bh = 4; *bytes = info.bpp;
    } else {
        *bw = 1; *bh = 1; *bytes = info.bpp;
    }
    return 1;
}

void vk_rt_dim_hint_record(int format, uint32_t width, uint32_t height) {
    VkRtDimHint *h = &g_rt_dim_hints[g_rt_dim_hint_next++ % 8];
    h->format = format;
    h->width = width;
    h->height = height;
}

int vk_rt_dim_hint_lookup(int format, uint32_t *width, uint32_t *height) {
    for (unsigned i = 0; i < 8; i++) {
        const VkRtDimHint *h = &g_rt_dim_hints[i];
        if (h->format != format || !h->width || !h->height) continue;
        if (width) *width = h->width;
        if (height) *height = h->height;
        return 1;
    }
    return 0;
}

VkImageAspectFlags vk_format_copy_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* A sampled texture view can only expose the depth aspect of a combined
 * depth/stencil format. */
VkImageAspectFlags vk_format_view_aspect(VkFormat f) {
    if (vk_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkSampleCountFlagBits vk_sample_count(uint32_t samples) {
    switch (samples) {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return (VkSampleCountFlagBits)0;
    }
}

int vk_format_can_texture(VkFormat f, VkFormatFeatureFlags *out_features) {
    if (f == VK_FORMAT_UNDEFINED || vk_format_info(f).bpp == 0) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &fp);
    if (out_features) *out_features = fp.optimalTilingFeatures;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

int vk_format_can_blit(VkFormat f, int dst) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.phys, f, &fp);
    VkFormatFeatureFlags need = dst ? VK_FORMAT_FEATURE_BLIT_DST_BIT
                                    : VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}
