/* Module: reflect_json. */
#include "loader_internal.h"

/* The generated .res.json files are simple, trusted arrays.  Read the
 * "cbufs" binding numbers without pulling a JSON dependency into the CLI. */
unsigned reflect_cbufs(const char *path, uint32_t *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    char *text = read_text_file(path, NULL);
    if (!text) { fprintf(stderr, "[totk-vk] ERROR read reflection %s\n", path); return 0; }
    const char *p = strstr(text, "\"cbufs\"");
    if (!p) { free(text); return 0; }
    p = strchr(p, '[');
    unsigned n = 0;
    while (p && n < max) {
        const char *obj = strchr(p, '{');
        const char *end = strchr(p, ']');
        if (!obj || (end && obj > end)) break;
        const char *close = strchr(obj, '}');
        if (!close) break;
        const char *b = strstr(obj, "\"binding\"");
        if (b && b < close) {
            b += strlen("\"binding\"");
            while (*b && (*b == ' ' || *b == ':' || *b == '\t')) b++;
            uint32_t v = (uint32_t)strtoul(b, NULL, 10);
            int dup = 0;
            for (unsigned i = 0; i < n; i++) if (out[i] == v) dup = 1;
            if (!dup) out[n++] = v;
        }
        p = close + 1;
    }
    free(text);
    return n;
}

/* The generated .res.json is trusted, flat JSON: arrays of small objects with
 * no nested objects.  These helpers locate one member without pulling a JSON
 * dependency into the CLI; bounds are always the current object's close brace. */
const char *json_member(const char *obj, const char *end, const char *key) {
    size_t klen = strlen(key);
    const char *p = obj;
    while (p < end) {
        p = memchr(p, '"', (size_t)(end - p));
        if (!p) return NULL;
        const char *q = memchr(p + 1, '"', (size_t)(end - p - 1));
        if (!q) return NULL;
        if ((size_t)(q - p - 1) == klen && !memcmp(p + 1, key, klen)) {
            const char *c = q + 1;
            while (c < end && (*c == ':' || *c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')) c++;
            return c < end ? c : NULL;
        }
        p = q + 1;
    }
    return NULL;
}

long json_long(const char *obj, const char *end, const char *key, long dflt) {
    const char *p = json_member(obj, end, key);
    if (!p) return dflt;
    char *e = NULL;
    long v = strtol(p, &e, 10);
    return e == p ? dflt : v;
}

int json_bool(const char *obj, const char *end, const char *key, int dflt) {
    const char *p = json_member(obj, end, key);
    if (!p) return dflt;
    if ((size_t)(end - p) >= 4 && !memcmp(p, "true", 4)) return 1;
    if ((size_t)(end - p) >= 5 && !memcmp(p, "false", 5)) return 0;
    return dflt;
}

void json_string(const char *obj, const char *end, const char *key,
                        char *out, size_t out_size) {
    if (out && out_size) out[0] = 0;
    const char *p = json_member(obj, end, key);
    if (!p || p >= end || *p != '"') return;
    p++;
    const char *q = memchr(p, '"', (size_t)(end - p));
    if (!q) return;
    size_t n = (size_t)(q - p);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

/* Ryujinx.Graphics.Shader.TextureFormat, in declaration order. */
uint32_t reflect_texture_format(const char *name) {
    static const char *const names[] = {
        "Unknown", "R8Unorm", "R8Snorm", "R8Uint", "R8Sint",
        "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint",
        "R32Float", "R32Uint", "R32Sint",
        "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint",
        "R16G16Float", "R16G16Unorm", "R16G16Snorm", "R16G16Uint", "R16G16Sint",
        "R32G32Float", "R32G32Uint", "R32G32Sint",
        "R8G8B8A8Unorm", "R8G8B8A8Snorm", "R8G8B8A8Uint", "R8G8B8A8Sint",
        "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm",
        "R16G16B16A16Uint", "R16G16B16A16Sint",
        "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint",
        "R10G10B10A2Unorm", "R10G10B10A2Uint", "R11G11B10Float",
    };
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i])) return i;
    return 0;
}

GfxBindingKind reflect_json_kind(const char *section, long type, int separate) {
    if (!strcmp(section, "cbufs")) return GFX_BIND_UBO;
    if (!strcmp(section, "sbufs")) return GFX_BIND_SSBO;
    if (!strcmp(section, "images"))
        return (type & 0xff) == 2 ? GFX_BIND_STORAGE_TEXEL_BUFFER : GFX_BIND_STORAGE_IMAGE;
    if (type == 0) return GFX_BIND_SAMPLER;
    if ((type & 0xff) == 2) return GFX_BIND_UNIFORM_TEXEL_BUFFER;
    return separate ? GFX_BIND_SAMPLED_IMAGE : GFX_BIND_COMBINED_IMAGE_SAMPLER;
}

unsigned reflect_json_section(const char *text, const char *section,
                                     GfxBindingSpec *out, unsigned max) {
    char key[64]; snprintf(key, sizeof key, "\"%s\"", section);
    const char *p = strstr(text, key);
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    const char *end_arr = strchr(p, ']');
    if (!end_arr) return 0;
    unsigned n = 0;
    while (p < end_arr && n < max) {
        const char *obj = strchr(p, '{');
        if (!obj || obj > end_arr) break;
        const char *close = strchr(obj, '}');
        if (!close || close > end_arr) break;

        long lb = json_long(obj, close, "binding", -1);
        if (lb >= 0) {
            GfxBindingSpec s; memset(&s, 0, sizeof s);
            s.binding = (uint32_t)lb;
            long type = json_long(obj, close, "type", 0);
            s.separate = json_bool(obj, close, "separate", 0);
            s.kind = reflect_json_kind(section, type, s.separate);
            s.count = 1;
            long array = json_long(obj, close, "array", 1);
            if (!strcmp(section, "textures") || !strcmp(section, "images"))
                s.count = array > 0 ? (uint32_t)array : 0;
            s.nvn_slot = (int32_t)json_long(obj, close, "slot", -1);
            s.nvn_cbuf_slot = (int32_t)json_long(obj, close, "cbuf_slot",
                                                json_long(obj, close, "cb_slot", -1));
            s.nvn_cb_offset = (int32_t)json_long(obj, close, "cb_offset", 0);
            s.nvn_handle = (uint32_t)json_long(obj, close, "handle", 0);
            s.nvn_type = (uint32_t)type;
            s.nvn_flags = (uint32_t)json_long(obj, close, "flags", 0);
            char fmt[48]; json_string(obj, close, "format", fmt, sizeof fmt);
            s.nvn_format = reflect_texture_format(fmt);
            /* The translated immediate is the texture slot in the default c2
             * table (cbuf_slot < 0), or the encoded table offset when the
             * translator named c2 explicitly.  Keep both forms. */
            if ((!strcmp(section, "textures") || !strcmp(section, "images")) &&
                s.nvn_slot < 0 && s.nvn_handle && (s.nvn_cbuf_slot < 0 || s.nvn_cbuf_slot == 2)) {
                uint32_t off0 = s.nvn_handle & 0x3fffu;
                uint32_t bytes = off0 * 4u;
                if (bytes >= 0x168u && bytes < 0x568u) s.nvn_slot = (int32_t)((bytes - 0x168u) / 8u);
                else if (off0 < 128u) s.nvn_slot = (int32_t)off0;
            }

            int dup = 0;
            for (unsigned i = 0; i < n; i++)
                if (out[i].binding == s.binding) { dup = 1; break; }
            if (!dup) out[n++] = s;
        }
        p = close + 1;
    }
    return n;
}

unsigned reflect_json_file(const char *path, GfxBindingSpec *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    char *text = read_text_file(path, NULL);
    if (!text) { fprintf(stderr, "[totk-vk] ERROR read reflection %s\n", path); return 0; }
    unsigned n = 0;
    n += reflect_json_section(text, "cbufs", out + n, max - n);
    n += reflect_json_section(text, "sbufs", out + n, max - n);
    n += reflect_json_section(text, "textures", out + n, max - n);
    n += reflect_json_section(text, "images", out + n, max - n);
    free(text);
    return n;
}

/* SPIR-V owns the descriptor type and array length; .res.json only supplies
 * the NVN source metadata (slot/handle/format/flags). */
unsigned reflect_merge(GfxBindingSpec *out, unsigned max,
                              const GfxBindingSpec *json, unsigned njson,
                              const GfxBindingSpec *spv, unsigned nspv) {
    unsigned n = 0;
    for (unsigned i = 0; i < nspv && n < max; i++) {
        GfxBindingSpec s = spv[i];
        for (unsigned j = 0; j < njson; j++) {
            if (json[j].binding != s.binding) continue;
            s.nvn_slot = json[j].nvn_slot;
            s.nvn_cbuf_slot = json[j].nvn_cbuf_slot;
            s.nvn_cb_offset = json[j].nvn_cb_offset;
            s.nvn_handle = json[j].nvn_handle;
            s.nvn_type = json[j].nvn_type;
            s.nvn_format = json[j].nvn_format;
            s.nvn_flags = json[j].nvn_flags;
            s.separate = json[j].separate;
            break;
        }
        out[n++] = s;
    }
    return n;
}

unsigned gfx_reflect_full(const char *json_path, const char *spv_path, uint32_t target_set,
                          GfxBindingSpec *out, unsigned max) {
    GfxBindingSpec json[64], spv[64];
    unsigned njson = reflect_json_file(json_path, json, 64);
    unsigned nspv = reflect_spirv_file(spv_path, target_set, spv, 64);
    if (!nspv) {
        for (unsigned i = 0; i < njson && i < max; i++) { json[i].set = target_set; out[i] = json[i]; }
        return njson < max ? njson : max;
    }
    return reflect_merge(out, max, json, njson, spv, nspv);
}

const char *reflect_kind_name(GfxBindingKind k) {
    switch (k) {
    case GFX_BIND_UBO: return "ubo";
    case GFX_BIND_SSBO: return "ssbo";
    case GFX_BIND_SAMPLER: return "sampler";
    case GFX_BIND_SAMPLED_IMAGE: return "sampled-image";
    case GFX_BIND_COMBINED_IMAGE_SAMPLER: return "combined-sampler";
    case GFX_BIND_STORAGE_IMAGE: return "storage-image";
    case GFX_BIND_UNIFORM_TEXEL_BUFFER: return "uniform-texel-buffer";
    case GFX_BIND_STORAGE_TEXEL_BUFFER: return "storage-texel-buffer";
    default: return "?";
    }
}

int reflect_has_non_ubo(const char *path) {
    GfxBindingSpec s[64];
    unsigned n = reflect_json_file(path, s, 64);
    for (unsigned i = 0; i < n; i++)
        if (s[i].kind != GFX_BIND_UBO) return 1;
    return 0;
}

void reflect_log(const char *stage, const GfxBindingSpec *s, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "[totk-vk] reflect %s set=%u binding=%u kind=%s count=%u"
                        " nvn(slot=%d cbuf=%d off=%d handle=%#x type=%#x fmt=%u flags=%#x separate=%d)\n",
                stage, s[i].set, s[i].binding, reflect_kind_name(s[i].kind), s[i].count,
                s[i].nvn_slot, s[i].nvn_cbuf_slot, s[i].nvn_cb_offset, s[i].nvn_handle,
                s[i].nvn_type, s[i].nvn_format, s[i].nvn_flags, s[i].separate);
}
