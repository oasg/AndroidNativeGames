/* Offline shader cache for the Android NVN HLE.
 *
 * The guest still hands NVN the Maxwell control/data pair.  The key is the
 * same SHA-256 the macOS shader seed uses: the control section bytes NVN
 * hashes (0x880, or its own declared size) followed by the shader code up to
 * the length in the control section.  SPIR-V and its .res.json reflection are
 * produced offline on the macOS host and copied into this cache; the Android
 * process never runs a .NET translator.
 *
 * Layout (all paths under the cache root):
 *
 *   <stage>/<key>.spv          canonical entry written by the packer
 *   <stage>/<key>.res.json     optional descriptor reflection
 *   <stage>/<key>-<variant>.spv / .res.json
 *   <key>.spv / <key>.res.json flat fallback
 *
 * The variant form keeps the seed's <key>-<bind-suffix> naming usable.
 */
#ifndef TOTK_ANDROID_SHADER_CACHE_H
#define TOTK_ANDROID_SHADER_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define RUNTIME_SHADER_KEY_MAX 64
#define RUNTIME_SHADER_PATH_MAX 1024
#define RUNTIME_SHADER_STAGES 6

typedef struct {
    uint32_t stage;              /* NVN stage: 0 vs, 1 ps, 5 cs */
    uint32_t control_key_bytes;  /* bytes of the control section hashed */
    uint32_t code_bytes;         /* bytes of code hashed */
    char key[RUNTIME_SHADER_KEY_MAX + 1];
} RuntimeShaderIdentity;

/* The runtime key for one NVN shader.  Returns 0 when the control/code pair
 * does not describe a valid NVN shader.  This does not touch the disk. */
int runtime_shader_identity(const void *control, const void *code,
                            RuntimeShaderIdentity *out);
const char *runtime_shader_stage_name(uint32_t stage);

/* Resolve an NVNshaderData code address.
 *
 * TOTK's agl shader packages (BAHS) store the code section as an offset into
 * the package's code pool.  The guest sometimes hands NVN that pool offset in
 * the low NVNbufferAddress word (for example 0x16300) while `control` is a
 * normal host pointer into the same package mapping, so the code pointer must
 * be recovered as:
 *
 *     package_base + pool_offset + low_address
 *
 * where package_base is the BAHS header that precedes `control` in its
 * mapping.  Genuine pointers (>= 0x100000) are passed through.  Resolution is
 * rejected unless the candidate lands in the same readable mapping, fits the
 * control section's declared code length, and starts with the SPH magic.
 */
typedef struct {
    uintptr_t region_start;
    uintptr_t region_end;
    uintptr_t package_base;   /* BAHS header used for the offset, 0 if none */
    uint32_t pool_offset;
    uint32_t pool_size;
    uintptr_t candidate;      /* last candidate examined */
    uint32_t code_bytes;      /* control section's declared length */
    const char *why;          /* failure reason / "bahs-pool" / "absolute" */
} RuntimeShaderResolveInfo;

/* Pure resolver: `region` is a readable mapping that contains `control`.
 * Exposed for the host-side regression test; runtime_shader_resolve_code()
 * finds the mapping in /proc/self/maps and calls this. */
int runtime_shader_resolve_in_region(const void *region, size_t region_size,
                                     const void *control, uint64_t low_addr,
                                     const void **code, RuntimeShaderResolveInfo *info);
int runtime_shader_resolve_code(const void *control, uint64_t low_addr,
                                const void **code, RuntimeShaderResolveInfo *info);

/* Cache root: TOTK_ANDROID_SHADER_CACHE, else <profile>/shader-cache, else
 * /data/local/tmp/totk-android/shader-cache.  The returned string is static. */
const char *runtime_shader_cache_root(void);

typedef struct {
    uint32_t stage;
    uint8_t *spv;
    size_t spv_size;
    char spv_path[RUNTIME_SHADER_PATH_MAX];
    char res_path[RUNTIME_SHADER_PATH_MAX];  /* empty when no reflection */
} RuntimeShaderModule;

/* Load <key>.spv (and <key>.res.json when present).  Returns 1 on a valid
 * SPIR-V module; the caller owns `spv`.  A miss is not an error and is logged
 * at the call site, not here. */
int runtime_shader_cache_load(const char *root, const RuntimeShaderIdentity *id,
                              RuntimeShaderModule *out);
/* Load <key>-<suffix>.spv, falling back to the canonical <key>.spv. */
int runtime_shader_cache_load_variant(const char *root, const RuntimeShaderIdentity *id,
                                      const char *suffix, RuntimeShaderModule *out);
/* 1 when at least one <key>-*.spv exists (cheap directory scan). */
int runtime_shader_cache_has_variants(const char *root, const RuntimeShaderIdentity *id);
void runtime_shader_cache_module_free(RuntimeShaderModule *module);

/* Persist one translated module.  Used by the cache probe and by host-side
 * tooling; a missing `res_json` only writes the .spv file. */
int runtime_shader_cache_store(const char *root, const RuntimeShaderIdentity *id,
                               const uint8_t *spv, size_t spv_size,
                               const char *res_json);

#endif
