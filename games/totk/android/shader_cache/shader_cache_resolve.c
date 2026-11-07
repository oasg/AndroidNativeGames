/* Module: resolve. */
#include "shader_cache_internal.h"

static const char *const g_stage_names[RUNTIME_SHADER_STAGES] = {
    "vs", "ps", "gs", "tcs", "tes", "cs",
};

const char *runtime_shader_stage_name(uint32_t stage) {
    return stage < RUNTIME_SHADER_STAGES ? g_stage_names[stage] : "?";
}

int is_spv(const uint8_t *p, size_t n) {
    return p && n >= 20 && !(n & 3) && !memcmp(p, "\x03\x02\x23\x07", 4);
}

/* ------------------------------------------------------------------------- */
/* Low NVNbufferAddress resolution                                            */


int control_declared_length(const void *control_v, uintptr_t region_start,
                                   size_t region_size, uint32_t *length_out,
                                   uint32_t *stage_out) {
    uintptr_t control = (uintptr_t)control_v;
    if (!control || control < region_start || control - region_start > region_size ||
        region_size - (control - region_start) < 0x718)
        return 0;
    const uint8_t *c = (const uint8_t *)control_v;
    uint32_t declared, c1, length, stage;
    memcpy(&declared, c + 0x20, sizeof declared);
    uint32_t key_len = declared >= 0x7cc && declared < 0x880 ? declared : 0x880;
    memcpy(&c1, c + 0x700, sizeof c1);
    memcpy(&length, c + 0x704, sizeof length);
    memcpy(&stage, c + 0x714, sizeof stage);
    if (key_len < 0x718 || key_len > 0x880 || stage >= RUNTIME_SHADER_STAGES ||
        !length || length > SHADER_SPV_MAX || c1 > length)
        return 0;
    if (region_size - (control - region_start) < key_len) return 0;
    *length_out = length;
    if (stage_out) *stage_out = stage;
    return 1;
}

/* The code section starts with a 0x30-byte SPH header (magic + reserved
 * zeros).  Graphics shaders use 0x12345678 and compute shaders 0x12345679
 * (verified against the runtime's own .code dumps for vs/ps/cs).  The magic
 * alone is not enough: require a non-empty instruction after the header when
 * the declared length has room for one. */
int sph_ok(const uint8_t *region, size_t region_size, size_t off,
                  uint32_t length, uint32_t stage) {
    if (off > region_size || length > region_size - off || length < 4 ||
        (off & 3))
        return 0;
    uint32_t magic;
    memcpy(&magic, region + off, sizeof magic);
    uint32_t expected = stage == 5 ? 0x12345679u : SHADER_SPH_MAGIC;
    if (magic != expected) return 0;
    size_t body = off + (length < 0x30 ? length : 0x30);
    for (size_t i = off + 4; i < body; i++)
        if (region[i]) return 0;          /* reserved header bytes are zero */
    if (length > 0x30) {
        int nonzero = 0;
        for (size_t i = off + 0x30; i < off + length; i++)
            if (region[i]) { nonzero = 1; break; }
        if (!nonzero) return 0;
    }
    return 1;
}

/* Nearest BAHS package header before `control`, with `control` in its record
 * area.  The code pool is at header + pool and its size is pool_size. */
int find_bahs(const uint8_t *region, size_t region_size, size_t control_off,
                     size_t *header_out, uint32_t *pool_out, uint32_t *pool_size_out) {
    const size_t step = 64 * 1024;
    size_t hi = control_off;
    while (hi > 0) {
        size_t lo = hi > step ? hi - step : 0;
        size_t i = hi;
        while (i >= lo + 4) {
            size_t h = i - 4;
            if (!memcmp(region + h, "BAHS", 4) && h + 32 <= region_size) {
                uint32_t rec_start, pool_size, pool;
                memcpy(&rec_start, region + h + 20, sizeof rec_start);
                memcpy(&pool_size, region + h + 24, sizeof pool_size);
                memcpy(&pool, region + h + 28, sizeof pool);
                uint64_t end = (uint64_t)h + pool + pool_size;
                if (rec_start < 0x20 || !pool_size || pool < 0x40 ||
                    pool_size > (256u << 20) || end > region_size)
                    goto next;
                if (control_off < (uint64_t)h + rec_start ||
                    control_off + 0x880 > (uint64_t)h + pool)
                    goto next;
                *header_out = h;
                *pool_out = pool;
                *pool_size_out = pool_size;
                return 1;
            }
        next:
            if (i == lo + 4) break;
            i--;
        }
        if (lo == 0) break;
        hi = lo + 3;
    }
    return 0;
}

int resolve_in_region_impl(const void *region_v, size_t region_size,
                                  const void *control, uint64_t low_addr,
                                  const void **code, RuntimeShaderResolveInfo *info,
                                  int pointer_ok) {
    RuntimeShaderResolveInfo local;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    info->region_start = (uintptr_t)region_v;
    info->region_end = (uintptr_t)region_v + region_size;
    info->candidate = (uintptr_t)low_addr;
    if (code) *code = NULL;
    if (!region_v || !control || !code) { info->why = "invalid-args"; return 0; }

    /* A genuine host pointer needs no translation, but only when the caller has
     * already verified that the address is mapped.  TOTK also passes BAHS code
     * offsets above 1 MiB; those must go through the pool resolver. */
    if (pointer_ok && low_addr >= SHADER_LOW_PTR) {
        uintptr_t ptr = (uintptr_t)low_addr & 0x00ffffffffffffffULL;
        info->candidate = ptr;
        info->why = "pointer";
        *code = (const void *)ptr;
        return 1;
    }

    uintptr_t control_addr = (uintptr_t)control & 0x00ffffffffffffffULL;
    if (control_addr < (uintptr_t)region_v ||
        control_addr - (uintptr_t)region_v >= region_size) {
        info->why = "control-outside-region";
        return 0;
    }
    size_t control_off = control_addr - (uintptr_t)region_v;
    uint32_t length = 0, stage = 0;
    if (!control_declared_length(control, (uintptr_t)region_v, region_size, &length,
                                 &stage)) {
        info->why = "bad-control";
        return 0;
    }
    info->code_bytes = length;

    const uint8_t *region = region_v;
    size_t header = 0;
    uint32_t pool = 0, pool_size = 0;
    if (find_bahs(region, region_size, control_off, &header, &pool, &pool_size)) {
        info->package_base = (uintptr_t)region_v + header;
        info->pool_offset = pool;
        info->pool_size = pool_size;
        if (low_addr < pool_size && low_addr + length <= pool_size) {
            size_t off = header + pool + (size_t)low_addr;
            info->candidate = (uintptr_t)region_v + off;
            if (sph_ok(region, region_size, off, length, stage)) {
                info->why = "bahs-pool";
                *code = region + off;
                return 1;
            }
            info->why = "bahs-pool-bad-code";
        } else {
            info->why = "bahs-pool-range";
        }
    } else {
        info->why = "no-bahs-header";
    }

    /* Fallback: some packages map code at the mapping base itself.  Accept it
     * only when the candidate passes the same SPH/length validation. */
    if (low_addr + length <= region_size) {
        info->candidate = (uintptr_t)region_v + (size_t)low_addr;
        if (sph_ok(region, region_size, (size_t)low_addr, length, stage)) {
            info->why = "mapping-absolute";
            *code = region + (size_t)low_addr;
            return 1;
        }
    }
    return 0;
}

int runtime_shader_resolve_in_region(const void *region_v, size_t region_size,
                                     const void *control, uint64_t low_addr,
                                     const void **code, RuntimeShaderResolveInfo *info) {
    return resolve_in_region_impl(region_v, region_size, control, low_addr, code, info, 1);
}

int runtime_shader_resolve_code(const void *control, uint64_t low_addr,
                                const void **code, RuntimeShaderResolveInfo *info) {
    RuntimeShaderResolveInfo local;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    if (code) *code = NULL;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { info->why = "no-maps"; return 0; }
    char line[512];
    /* Bionic may tag heap pointers; /proc/self/maps uses untagged VAs. */
    uintptr_t control_addr = (uintptr_t)control & 0x00ffffffffffffffULL;
    uintptr_t code_addr = (uintptr_t)low_addr & 0x00ffffffffffffffULL;
    int found = 0, code_mapped = 0;
    unsigned long long code_start = 0, code_end = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = { 0 };
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        if (low_addr >= SHADER_LOW_PTR && code_addr >= start && code_addr < end &&
            perms[0] == 'r') {
            code_mapped = 1;
            code_start = start;
            code_end = end;
        }
        if (control_addr < start || control_addr >= end) continue;
        found = 1;
        if (perms[0] != 'r') { info->why = "control-not-readable"; break; }
        if (low_addr >= SHADER_LOW_PTR && code_mapped) {
            info->candidate = code_addr;
            info->region_start = code_start;
            info->region_end = code_end;
            info->why = "pointer";
            if (code) *code = (const void *)code_addr;
            break;
        }
        /* The value is not a mapped host pointer.  It can still be a BAHS code
         * offset above 1 MiB; resolve it through the control's package pool. */
        resolve_in_region_impl((const void *)(uintptr_t)start,
                               (size_t)(end - start), control, low_addr,
                               code, info, 0);
        break;
    }
    fclose(f);
    if (!*code) {
        if (!found && !info->why) info->why = "control-not-mapped";
        else if (found && low_addr >= SHADER_LOW_PTR && !code_mapped && !info->why)
            info->why = "pointer-unmapped";
    }
    return *code != NULL;
}

int runtime_shader_identity(const void *control_v, const void *code_v,
                            RuntimeShaderIdentity *out) {
    const uint8_t *control = control_v, *code = code_v;
    if (!control || !code || !out) return 0;
    /* A malformed NVNshaderData entry can carry a small integer where the
     * control/code pointer belongs.  Never hash through an unmapped low
     * address; the caller logs the entry and treats the stage as missing. */
    if ((uintptr_t)control < 0x100000 || (uintptr_t)code < 0x100000) return 0;

    uint32_t declared, c1, length, stage;
    memcpy(&declared, control + 0x20, sizeof declared);
    /* Same rule as prepare_shader_seed.py control_key_len(): the section's own
     * size when it is a short self-contained BOTW/old-BNSH section, otherwise
     * the 0x880 TOTK NVN control block. */
    uint32_t key_len = declared >= 0x7cc && declared < 0x880 ? declared : 0x880;
    memcpy(&c1, control + 0x700, sizeof c1);
    memcpy(&length, control + 0x704, sizeof length);
    memcpy(&stage, control + 0x714, sizeof stage);
    if (key_len < 0x718 || key_len > 0x880 || stage >= RUNTIME_SHADER_STAGES ||
        !length || length > SHADER_SPV_MAX || c1 > length)
        return 0;

    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, control, key_len);
    sha256_update(&h, code, length);
    uint8_t digest[32];
    sha256_final(&h, digest);
    const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof digest; i++) {
        out->key[2 * i] = hex[digest[i] >> 4];
        out->key[2 * i + 1] = hex[digest[i] & 15];
    }
    out->key[64] = 0;
    out->stage = stage;
    out->control_key_bytes = key_len;
    out->code_bytes = length;
    return 1;
}
