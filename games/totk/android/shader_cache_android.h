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
void runtime_shader_cache_module_free(RuntimeShaderModule *module);

/* Persist one translated module.  Used by the cache probe and by host-side
 * tooling; a missing `res_json` only writes the .spv file. */
int runtime_shader_cache_store(const char *root, const RuntimeShaderIdentity *id,
                               const uint8_t *spv, size_t spv_size,
                               const char *res_json);

#endif
