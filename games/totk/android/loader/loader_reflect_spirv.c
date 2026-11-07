/* Module: reflect_spirv. */
#include "loader_internal.h"

int reflect_spirv_file(const char *path, uint32_t target_set,
                              GfxBindingSpec *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    size_t size = 0;
    uint8_t *raw = (uint8_t *)read_text_file(path, &size);
    if (!raw) { fprintf(stderr, "[totk-vk] ERROR read SPIR-V %s\n", path); return 0; }
    int ok = 0;
    if (size < 20 || (size & 3) || memcmp(raw, "\x03\x02\x23\x07", 4)) {
        fprintf(stderr, "[totk-vk] ERROR bad SPIR-V %s\n", path);
        free(raw);
        return 0;
    }
    uint32_t *w = (uint32_t *)raw;
    uint32_t bound = w[3];
    if (!bound || bound > REFLECT_SPV_MAX_BOUND) { free(raw); return 0; }

    ReflectSpvType *types = calloc(bound, sizeof *types);
    ReflectSpvVar *vars = calloc(bound, sizeof *vars);
    uint32_t *dset = malloc(bound * sizeof *dset);
    uint32_t *dbind = malloc(bound * sizeof *dbind);
    uint32_t *dblock = calloc(bound, sizeof *dblock);
    uint32_t *consts = calloc(bound, sizeof *consts);
    uint8_t *const_ok = calloc(bound, 1);
    if (!types || !vars || !dset || !dbind || !dblock || !consts || !const_ok) goto done;
    for (uint32_t i = 0; i < bound; i++) { dset[i] = UINT32_MAX; dbind[i] = UINT32_MAX; }
    unsigned nvars = 0;

    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        const uint32_t *o = w + i + 1;
        switch (op) {
        case 25: /* OpTypeImage */
            if (wc >= 9) {
                uint32_t id = o[0];
                if (id < bound) {
                    types[id].kind = 1;
                    types[id].dim = o[2]; types[id].depth = o[3];
                    types[id].arrayed = o[4]; types[id].ms = o[5];
                    types[id].sampled = o[6]; types[id].format = o[7];
                }
            }
            break;
        case 26: /* OpTypeSampler */
            if (wc >= 2 && o[0] < bound) types[o[0]].kind = 2;
            break;
        case 27: /* OpTypeSampledImage */
            if (wc >= 3 && o[0] < bound) { types[o[0]].kind = 3; types[o[0]].elem = o[1]; }
            break;
        case 28: /* OpTypeArray */
            if (wc >= 4 && o[0] < bound) {
                types[o[0]].kind = 4; types[o[0]].elem = o[1]; types[o[0]].length = o[2];
            }
            break;
        case 29: /* OpTypeRuntimeArray */
            if (wc >= 3 && o[0] < bound) { types[o[0]].kind = 5; types[o[0]].elem = o[1]; }
            break;
        case 30: /* OpTypeStruct */
            if (wc >= 2 && o[0] < bound) types[o[0]].kind = 6;
            break;
        case 32: /* OpTypePointer */
            if (wc >= 4 && o[0] < bound) {
                types[o[0]].kind = 7; types[o[0]].storage = o[1]; types[o[0]].elem = o[2];
            }
            break;
        case 43: /* OpConstant */
        case 50: /* OpSpecConstant */
            if (wc >= 4 && o[1] < bound) { consts[o[1]] = o[2]; const_ok[o[1]] = 1; }
            break;
        case 59: /* OpVariable */
            if (wc >= 4 && nvars < bound) {
                vars[nvars++] = (ReflectSpvVar){ o[1], o[0], o[2] };
            }
            break;
        case 71: /* OpDecorate */
            /* Block (2) and BufferBlock (3) carry no value word, so their
             * instruction is only 3 words wide.  Requiring wc >= 4 for every
             * decoration left dblock[] clear and classified every legacy
             * BufferBlock SSBO as a UBO. */
            if (wc >= 3 && o[0] < bound) {
                if (wc >= 4 && o[1] == 33) dbind[o[0]] = o[2];       /* Binding */
                else if (wc >= 4 && o[1] == 34) dset[o[0]] = o[2];   /* DescriptorSet */
                else if (o[1] == 2) dblock[o[0]] = 2;                /* Block */
                else if (o[1] == 3) dblock[o[0]] = 3;                /* BufferBlock */
            }
            break;
        default:
            break;
        }
        i += wc;
    }

    unsigned n = 0;
    for (unsigned v = 0; v < nvars && n < max; v++) {
        uint32_t id = vars[v].id, ptr = vars[v].ptr;
        if (id >= bound || ptr >= bound || dset[id] == UINT32_MAX || dbind[id] == UINT32_MAX)
            continue;
        if (types[ptr].kind != 7) continue;
        uint32_t t = types[ptr].elem;
        uint32_t count = 1;
        for (unsigned guard = 0; guard < 8 && t < bound; guard++) {
            uint32_t k = types[t].kind;
            if (k == 4) {
                uint32_t len = types[t].length < bound && const_ok[types[t].length]
                                   ? consts[types[t].length] : 0;
                count = len;
                t = types[t].elem;
            } else if (k == 5) {
                count = 0;
                t = types[t].elem;
            } else break;
        }
        if (t >= bound) continue;
        GfxBindingSpec s; memset(&s, 0, sizeof s);
        s.set = target_set;
        s.binding = dbind[id];
        s.count = count;   /* 0 = runtime array; the backend uses the pool size */
        uint32_t k = types[t].kind;
        if (k == 3) { /* sampled image: Buffer dimension is a texel buffer */
            uint32_t img = types[t].elem;
            s.kind = (img < bound && types[img].kind == 1 && types[img].dim == 5)
                         ? GFX_BIND_UNIFORM_TEXEL_BUFFER
                         : GFX_BIND_COMBINED_IMAGE_SAMPLER;
        } else if (k == 2) {
            s.kind = GFX_BIND_SAMPLER;
        } else if (k == 1) {
            int buffer = types[t].dim == 5;
            int storage = types[t].sampled == 2 || (types[t].sampled != 1 && types[t].format != 0);
            s.kind = buffer ? (storage ? GFX_BIND_STORAGE_TEXEL_BUFFER : GFX_BIND_UNIFORM_TEXEL_BUFFER)
                            : (storage ? GFX_BIND_STORAGE_IMAGE : GFX_BIND_SAMPLED_IMAGE);
        } else if (k == 6) {
            /* SPIR-V 1.3 uses StorageBuffer (12) for SSBOs; older frontends use
             * Uniform (2) plus the legacy BufferBlock decoration. */
            s.kind = (types[ptr].storage == 12 || dblock[t] == 3) ? GFX_BIND_SSBO : GFX_BIND_UBO;
        } else {
            /* Uniform variables point at a struct; if the type was not seen,
             * storage class 2 is still a uniform block. */
            s.kind = GFX_BIND_UBO;
        }
        int dup = 0;
        for (unsigned i = 0; i < n; i++)
            if (out[i].binding == s.binding) { dup = 1; break; }
        if (!dup) out[n++] = s;
    }
    ok = (int)n;

done:
    free(types); free(vars); free(dset); free(dbind); free(dblock);
    free(consts); free(const_ok); free(raw);
    return ok;
}

/* User-defined interface locations.  A fragment input with no matching vertex
 * output is not a valid pipeline test: some Adreno driver versions fail (or
 * appear to hang) inside vkCreateGraphicsPipelines before reporting an error. */
int spirv_interface_locations(const char *path, int fragment,
                                     uint32_t *out, unsigned max) {
    if (!path || !*path || !out || !max) return 0;
    size_t size = 0;
    uint8_t *raw = (uint8_t *)read_text_file(path, &size);
    if (!raw) return 0;
    if (size < 20 || (size & 3) || memcmp(raw, "\x03\x02\x23\x07", 4)) { free(raw); return 0; }
    uint32_t *w = (uint32_t *)raw, bound = w[3];
    if (!bound || bound > REFLECT_SPV_MAX_BOUND) { free(raw); return 0; }
    uint32_t *ptr_of = calloc(bound, sizeof *ptr_of);
    uint32_t *storage = calloc(bound, sizeof *storage);
    int32_t *location = malloc(bound * sizeof *location);
    uint8_t *builtin = calloc(bound, 1);
    uint32_t *entry_vars = calloc(bound, sizeof *entry_vars);
    if (!ptr_of || !storage || !location || !builtin || !entry_vars) {
        free(ptr_of); free(storage); free(location); free(builtin); free(entry_vars); free(raw);
        return 0;
    }
    for (uint32_t i = 0; i < bound; i++) location[i] = -1;
    unsigned nentry = 0, n = 0;

    for (size_t i = 5; i < size / 4;) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        const uint32_t *o = w + i + 1;
        if (op == 32 && wc >= 4 && o[0] < bound) {          /* OpTypePointer */
            storage[o[0]] = o[1];
        } else if (op == 59 && wc >= 4 && o[1] < bound) {   /* OpVariable */
            ptr_of[o[1]] = o[0];
        } else if (op == 71 && wc >= 4 && o[0] < bound) {   /* OpDecorate */
            if (o[1] == 30) location[o[0]] = (int32_t)o[2]; /* Location */
            else if (o[1] == 11) builtin[o[0]] = 1;         /* BuiltIn */
        } else if (op == 15 && wc >= 4) {                   /* OpEntryPoint */
            uint32_t model = o[0];
            if ((fragment && model != 4) || (!fragment && model != 0)) { i += wc; continue; }
            unsigned k = 2;  /* entry point name, null-terminated and padded */
            while (k < wc - 1) {
                uint32_t v = o[k++];
                if ((v & 0xff) == 0 || (v & 0xff00) == 0 ||
                    (v & 0xff0000) == 0 || (v & 0xff000000) == 0) break;
            }
            for (; k < wc - 1 && nentry < bound; k++) entry_vars[nentry++] = o[k];
        }
        i += wc;
    }

    for (unsigned i = 0; i < nentry && n < max; i++) {
        uint32_t id = entry_vars[i];
        if (id >= bound || builtin[id] || location[id] < 0) continue;
        uint32_t ptr = ptr_of[id];
        uint32_t want = fragment ? 1 : 3;   /* Input / Output */
        if (ptr >= bound || storage[ptr] != want) continue;
        int dup = 0;
        for (unsigned j = 0; j < n; j++) if (out[j] == (uint32_t)location[id]) dup = 1;
        if (!dup) out[n++] = (uint32_t)location[id];
    }
    free(ptr_of); free(storage); free(location); free(builtin); free(entry_vars); free(raw);
    return (int)n;
}

int reflect_check_interface(const char *vs_path, const char *ps_path) {
    uint32_t vs_out[64], ps_in[64];
    int nvs = spirv_interface_locations(vs_path, 0, vs_out, 64);
    int nps = spirv_interface_locations(ps_path, 1, ps_in, 64);
    int bad = 0;
    for (int i = 0; i < nps; i++) {
        int found = 0;
        for (int j = 0; j < nvs; j++) if (vs_out[j] == ps_in[i]) found = 1;
        if (!found) {
            fprintf(stderr, "[totk-vk] ERROR PS input location %u has no VS output "
                            "(pipeline interface mismatch; use a matching VS/PS pair)\n", ps_in[i]);
            bad = 1;
        }
    }
    return bad ? 0 : 1;
}

int gfx_spirv_move_set(uint8_t *code, size_t size, uint32_t from, uint32_t to) {
    if (!code || size < 20 || (size & 3)) return 0;
    uint32_t *w = (uint32_t *)code;
    for (size_t i = 5; i < size / 4; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > size / 4) break;
        /* OpDecorate target DescriptorSet value */
        if (op == 71 && wc >= 4 && w[i + 2] == 34 && w[i + 3] == from) w[i + 3] = to;
        i += wc;
    }
    return 1;
}
